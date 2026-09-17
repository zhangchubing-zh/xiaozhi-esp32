#include "../monitor.h"
#include "litecrab/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* AlarmPollingPath(void) {
    return "/get_monitor_info.asp?type=21&para1=%7B%22equipid%22%3A0%2C%22equipidlist%22%3A%5B%5D%"
           "7D&para2=&para3=&para4=&para5=&para6=";
}

static int append_identity_value(const char* row, const LjToken* token, LjBuf* out) {
    if (token->type == LJ_STRING) {
        char value[512];
        if (LjString(row, token, value, sizeof value))
            return -1;
        LjAppendJsonString(out, value);
        return out->failed ? -1 : 0;
    }
    if (token->type != LJ_PRIMITIVE)
        return -1;
    LjAppend(out, "%.*s", token->end - token->start, row + token->start);
    return out->failed ? -1 : 0;
}

static int field_text(const char* row,
                      const LjToken* tokens,
                      int count,
                      int object,
                      const char* name,
                      char* out,
                      size_t size) {
    int value = LjObjectGet(row, tokens, count, object, name);
    if (value < 0)
        return -1;
    if (tokens[value].type == LJ_STRING)
        return LjString(row, &tokens[value], out, size);
    if (tokens[value].type != LJ_PRIMITIVE)
        return -1;
    int length = tokens[value].end - tokens[value].start;
    if (length <= 0 || (size_t) length >= size)
        return -1;
    memcpy(out, row + tokens[value].start, (size_t) length);
    out[length] = 0;
    return 0;
}

static void append_value(LjBuf* message, const char* value, size_t limit) {
    size_t used = 0;
    const unsigned char* p = (const unsigned char*) value;
    while (*p) {
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            if (used + 1 > limit)
                break;
            LjAppend(message, " ");
            used++;
            p++;
            continue;
        }
        if (*p < 0x20) {
            p++;
            continue;
        }
        size_t bytes = 1;
        if ((*p & 0xe0) == 0xc0)
            bytes = 2;
        else if ((*p & 0xf0) == 0xe0)
            bytes = 3;
        else if ((*p & 0xf8) == 0xf0)
            bytes = 4;
        if (used + bytes > limit)
            break;
        LjAppend(message, "%.*s", (int) bytes, p);
        used += bytes;
        p += bytes;
    }
    if (*p)
        LjAppend(message, "...");
}

static void append_field(LjBuf* message, const char* label, const char* value, size_t limit) {
    if (!value || !*value)
        return;
    LjAppend(message, "%s ：", label);
    append_value(message, value, limit);
    LjAppend(message, ",\n");
}

static void optional_field(
    const char* row, const LjToken* tokens, int count, const char* name, char* out, size_t size) {
    if (field_text(row, tokens, count, 0, name, out, size))
        out[0] = 0;
}

static void selected_sub_reason(const char* row,
                                const LjToken* tokens,
                                int count,
                                char* reason,
                                size_t reasonSize,
                                char* repair,
                                size_t repairSize) {
    reason[0] = 0;
    repair[0] = 0;
    int list = LjObjectGet(row, tokens, count, 0, "subReasonList");
    if (list < 0 || tokens[list].type != LJ_ARRAY || tokens[list].size <= 0)
        return;
    int selected = 0;
    char position[32];
    if (!field_text(row, tokens, count, 0, "position", position, sizeof position)) {
        char* end = NULL;
        long value = strtol(position, &end, 10);
        if (end && !*end && value > 0 && value <= tokens[list].size)
            selected = (int) value - 1;
    }
    int item = LjArrayGet(tokens, count, list, selected);
    if (item < 0 || tokens[item].type != LJ_OBJECT)
        return;
    if (field_text(row, tokens, count, item, "subReason", reason, reasonSize))
        reason[0] = 0;
    if (field_text(row, tokens, count, item, "subRepair", repair, repairSize))
        repair[0] = 0;
}

/* One occurrence is stable across list reordering and mutable descriptions.
 * localtime is deliberately part of the key so a later recurrence is new. */
int AlarmOccurrenceParse(const char* row, AlarmNotify* out) {
    LjToken tokens[1024];
    LjParser parser;
    LjInit(&parser);
    int count = LjParse(&parser, row, strlen(row), tokens, 1024);
    if (count < 1 || tokens[0].type != LJ_OBJECT)
        return -1;
    const char* fields[] = {"equipid", "almid", "seqno", "localtime"};
    char identity[2048];
    LjBuf key;
    LjBufInit(&key, identity, sizeof identity);
    LjAppend(&key, "{\"almlist\":[{");
    for (int i = 0; i < 4; i++) {
        int value = LjObjectGet(row, tokens, count, 0, fields[i]);
        if (value < 0)
            return -1;
        if (i == 3) {
            char occurrenceTime[128];
            if (tokens[value].type != LJ_STRING ||
                LjString(row, &tokens[value], occurrenceTime, sizeof occurrenceTime) ||
                !occurrenceTime[0])
                return -1;
        } else if (tokens[value].type == LJ_STRING) {
            char identifier[128];
            if (LjString(row, &tokens[value], identifier, sizeof identifier) || !identifier[0])
                return -1;
        } else {
            int64_t identifier;
            if (LjInt64(row, &tokens[value], &identifier) || identifier < 0)
                return -1;
        }
        if (i)
            LjAppend(&key, ",");
        LjAppendJsonString(&key, fields[i]);
        LjAppend(&key, ":");
        if (append_identity_value(row, &tokens[value], &key))
            return -1;
    }
    LjAppend(&key, "}]}");
    if (key.failed || AlarmNotifyParse(identity, out))
        return -1;

    char equipId[128], alarmId[128], sequence[128], localTime[128];
    char alarmName[512], reasonCode[128], faultDescription[1024];
    char equipmentType[128], equipmentName[512], level[128];
    char subReason[1024], subRepair[1024];
    if (field_text(row, tokens, count, 0, "equipid", equipId, sizeof equipId) ||
        field_text(row, tokens, count, 0, "almid", alarmId, sizeof alarmId) ||
        field_text(row, tokens, count, 0, "seqno", sequence, sizeof sequence) ||
        field_text(row, tokens, count, 0, "localtime", localTime, sizeof localTime))
        return -1;
    optional_field(row, tokens, count, "almname", alarmName, sizeof alarmName);
    optional_field(row, tokens, count, "reason", reasonCode, sizeof reasonCode);
    optional_field(row, tokens, count, "faultDesc", faultDescription, sizeof faultDescription);
    optional_field(row, tokens, count, "equiptypeid", equipmentType, sizeof equipmentType);
    optional_field(row, tokens, count, "equipname", equipmentName, sizeof equipmentName);
    optional_field(row, tokens, count, "level", level, sizeof level);
    selected_sub_reason(
        row, tokens, count, subReason, sizeof subReason, subRepair, sizeof subRepair);

    LjBuf message;
    LjBufInit(&message, out->message, sizeof out->message);
    append_field(&message, "告警ID", alarmId, 64);
    append_field(&message, "告警名称", alarmName, 256);
    append_field(&message,
                 "告警原因",
                 faultDescription[0] ? faultDescription : (subReason[0] ? subReason : reasonCode),
                 512);
    append_field(&message, "告警原因代码", reasonCode, 64);
    append_field(&message, "设备ID", equipId, 64);
    append_field(&message, "设备类型ID", equipmentType, 64);
    append_field(&message, "设备名称", equipmentName, 256);
    append_field(&message, "告警序列号", sequence, 64);
    append_field(&message, "告警级别", level, 64);
    append_field(&message, "告警时间", localTime, 64);
    append_field(&message, "原因说明", subReason, 512);
    append_field(&message, "建议处理", subRepair, 512);
    LjAppend(&message, "使用 PLC_Diagnosis 完整告警诊断、修复和回归验证");
    return message.failed ? -1 : 0;
}
