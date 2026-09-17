#include "asp.h"
#include "litecrab/json.h"

#define NORMALIZED_MAX_ALARMS 20
#define TOP_CAUSES 3
#define CAUSE_TEXT_MAX 1024

typedef struct {
    const char *json;
    LjToken *tokens;
    int count;
} JsonDoc;

typedef struct {
    char reason[CAUSE_TEXT_MAX];
    char repair[CAUSE_TEXT_MAX];
} CausePair;

static void copy_limited(char *destination, size_t size, const char *source)
{
    if (!size) return;
    size_t length = source ? strnlen(source, size - 1U) : 0U;
    if (length) memcpy(destination, source, length);
    destination[length] = 0;
}

static int doc_open(JsonDoc *doc, const Buffer *response)
{
    memset(doc, 0, sizeof(*doc));
    if (!response || !response->data || LjValidate(response->data, LJ_OBJECT)) return -1;
    doc->tokens = calloc(response->size + 1U, sizeof(*doc->tokens));
    if (!doc->tokens) return -1;
    LjParser parser;
    LjInit(&parser);
    doc->count = LjParse(&parser, response->data, response->size, doc->tokens,
                         (unsigned)(response->size + 1U));
    if (doc->count < 1 || doc->tokens[0].type != LJ_OBJECT) {
        free(doc->tokens);
        memset(doc, 0, sizeof(*doc));
        return -1;
    }
    doc->json = response->data;
    return 0;
}

static void doc_close(JsonDoc *doc)
{
    free(doc->tokens);
    memset(doc, 0, sizeof(*doc));
}

static int field(const JsonDoc *doc, int object, const char *name)
{
    return LjObjectGet(doc->json, doc->tokens, doc->count, object, name);
}

static int field_int(const JsonDoc *doc, int object, const char *name, int64_t *value)
{
    int token = field(doc, object, name);
    return token >= 0 ? LjInt64(doc->json, &doc->tokens[token], value) : -1;
}

static char *token_string_alloc(const JsonDoc *doc, int token)
{
    if (token < 0 || doc->tokens[token].type != LJ_STRING) return NULL;
    size_t size = (size_t)(doc->tokens[token].end - doc->tokens[token].start) + 1U;
    char *value = malloc(size);
    if (!value || LjString(doc->json, &doc->tokens[token], value, size)) {
        free(value);
        return NULL;
    }
    return value;
}

static int token_int(const JsonDoc *doc, int token, int64_t *value)
{
    if (token < 0 || !value) return -1;
    if (doc->tokens[token].type != LJ_STRING)
        return LjInt64(doc->json, &doc->tokens[token], value);
    char *text = token_string_alloc(doc, token);
    if (!text) return -1;
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    int rc = errno || !end || *end ? -1 : 0;
    if (!rc) *value = (int64_t)parsed;
    free(text);
    return rc;
}

static void print_http(long status)
{
    if (status > 0) printf("%ld", status);
    else fputs("null", stdout);
}

static const char *error_kind(int rc)
{
    if (rc == NETWORK_ERROR) return "network";
    if (rc == HTTP_ERROR) return "http_or_business";
    if (rc == AUTH_ERROR) return "authentication";
    if (rc == ARG_ERROR) return "argument";
    if (rc == DIAGNOSIS_ERROR) return "verification";
    return "local";
}

void normalized_print_error(const char *operation, int rc, long status,
                            const char *stage, const char *message)
{
    fputs("{\"schema_version\":1,\"operation\":", stdout);
    json_string(operation);
    printf(",\"success\":false,\"exit_code\":%d,\"transport\":{\"http_status\":", rc);
    print_http(status);
    fputs("},\"data\":null,\"error\":{\"kind\":", stdout);
    json_string(error_kind(rc));
    fputs(",\"stage\":", stdout);
    json_string(stage ? stage : "request");
    fputs(",\"message\":", stdout);
    json_string(message ? message : "Operation failed");
    fputs("}}\n", stdout);
}

void normalized_print_login(int rc, long status)
{
    if (rc != OK) {
        normalized_print_error("login", rc, status, "login", "Authentication failed");
        return;
    }
    fputs("{\"schema_version\":1,\"operation\":\"login\",\"success\":true,"
          "\"exit_code\":0,\"transport\":{\"http_status\":", stdout);
    print_http(status);
    fputs("},\"data\":{\"authenticated\":true},\"error\":null}\n", stdout);
}

void normalized_print_cache(const Options *options, const Buffer *response, int rc)
{
    (void)options;
    if (rc != OK) {
        normalized_print_error("get_cache_info", rc, response ? response->http_status : 0,
                               "query", "Cache query failed");
        return;
    }
    const char *type = "text";
    if (response && response->data) {
        const char *p = response->data;
        while (*p && isspace((unsigned char)*p)) ++p;
        if (*p == '{') type = "object";
        else if (*p == '[') type = "array";
    }
    fputs("{\"schema_version\":1,\"operation\":\"get_cache_info\",\"success\":true,"
          "\"exit_code\":0,\"transport\":{\"http_status\":", stdout);
    print_http(response ? response->http_status : 0);
    fputs("},\"business\":{\"code\":null},\"query\":{\"requested_device_ids\":[]},"
          "\"data\":{\"schema_known\":false,\"response_type\":", stdout);
    json_string(type);
    printf(",\"response_bytes\":%zu},\"warnings\":["
           "\"cache response schema is not configured; raw body was not sent to the LLM\"],"
           "\"error\":null}\n", response ? response->size : 0U);
}

static void trim_item(char *text)
{
    char *start = text;
    while (*start && isspace((unsigned char)*start)) ++start;
    char *p = start;
    while (isdigit((unsigned char)*p)) ++p;
    if (p > start && (*p == '.' || *p == ')')) {
        ++p;
        while (*p && isspace((unsigned char)*p)) ++p;
        start = p;
    }
    if (start != text) memmove(text, start, strlen(start) + 1U);
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1U])) text[--n] = 0;
}

static int split_numbered(const char *text, char items[TOP_CAUSES][CAUSE_TEXT_MAX])
{
    int count = 0;
    const char *p = text ? text : "";
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        char item[CAUSE_TEXT_MAX];
        if (length >= sizeof(item)) length = sizeof(item) - 1U;
        memcpy(item, p, length);
        item[length] = 0;
        trim_item(item);
        if (item[0]) {
            if (count < TOP_CAUSES) copy_limited(items[count], CAUSE_TEXT_MAX, item);
            ++count;
        }
        p = end ? end + 1 : p + strlen(p);
    }
    return count;
}

static int cause_pairs(const JsonDoc *doc, int alarm, CausePair pairs[TOP_CAUSES], int *total)
{
    int pair_count = 0;
    *total = 0;
    int list = field(doc, alarm, "subReasonList");
    if (list >= 0 && doc->tokens[list].type == LJ_ARRAY) {
        for (int i = 0; i < doc->tokens[list].size; ++i) {
            int item = LjArrayGet(doc->tokens, doc->count, list, i);
            char *reason = token_string_alloc(doc, field(doc, item, "subReason"));
            char *repair = token_string_alloc(doc, field(doc, item, "subRepair"));
            char reasons[TOP_CAUSES][CAUSE_TEXT_MAX] = {{0}};
            char repairs[TOP_CAUSES][CAUSE_TEXT_MAX] = {{0}};
            int reason_count = split_numbered(reason, reasons);
            int repair_count = split_numbered(repair, repairs);
            int local_count = reason_count > repair_count ? reason_count : repair_count;
            *total += local_count;
            for (int j = 0; j < local_count && pair_count < TOP_CAUSES; ++j) {
                if (j < reason_count) copy_limited(pairs[pair_count].reason, CAUSE_TEXT_MAX, reasons[j]);
                if (j < repair_count) copy_limited(pairs[pair_count].repair, CAUSE_TEXT_MAX, repairs[j]);
                ++pair_count;
            }
            free(reason);
            free(repair);
        }
    }
    if (pair_count) return pair_count;

    /* Fallback: description contains a catalog of "Fault Code N" reason blocks.
     * It has no reliable repair pairing, so return reasons with repair=null. */
    int64_t reason_number;
    char *description = token_string_alloc(doc, field(doc, alarm, "description"));
    if (!description || field_int(doc, alarm, "reasonum", &reason_number)) {
        free(description);
        return 0;
    }
    char marker[64];
    snprintf(marker, sizeof(marker), "Fault Code %lld:", (long long)reason_number);
    char *start = strstr(description, marker);
    if (!start) {
        free(description);
        return 0;
    }
    start += strlen(marker);
    while (*start && isspace((unsigned char)*start)) ++start;
    if (start[0] == '1' && start[1] == ')') start += 2;
    char *end = strstr(start, "<br/>Fault Code ");
    if (end) *end = 0;
    char reasons[TOP_CAUSES][CAUSE_TEXT_MAX] = {{0}};
    *total = split_numbered(start, reasons);
    pair_count = *total > TOP_CAUSES ? TOP_CAUSES : *total;
    for (int i = 0; i < pair_count; ++i)
        copy_limited(pairs[i].reason, CAUSE_TEXT_MAX, reasons[i]);
    free(description);
    return pair_count;
}

static void print_optional_field(const JsonDoc *doc, int object, const char *source,
                                 const char *target, int *emitted)
{
    int token = field(doc, object, source);
    if (token < 0) return;
    if (*emitted) putchar(',');
    json_string(target);
    putchar(':');
    if (doc->tokens[token].type == LJ_STRING) {
        char *value = token_string_alloc(doc, token);
        if (value) json_string(value); else fputs("null", stdout);
        free(value);
    } else {
        fwrite(doc->json + doc->tokens[token].start, 1U,
               (size_t)(doc->tokens[token].end - doc->tokens[token].start), stdout);
    }
    *emitted = 1;
}

void normalized_print_active_alarm(const Options *options, const Buffer *response, int rc)
{
    if (rc != OK) {
        normalized_print_error("get_active_alarm", rc, response ? response->http_status : 0,
                               "query", "Active alarm query failed");
        return;
    }
    JsonDoc doc;
    if (doc_open(&doc, response)) {
        normalized_print_error("get_active_alarm", HTTP_ERROR, response->http_status,
                               "normalize_response", "Invalid active alarm response");
        return;
    }
    int list = field(&doc, 0, "almlist");
    int code = field(&doc, 0, "errcode");
    int total = list >= 0 && doc.tokens[list].type == LJ_ARRAY ? doc.tokens[list].size : 0;
    int returned = total > NORMALIZED_MAX_ALARMS ? NORMALIZED_MAX_ALARMS : total;
    fputs("{\"schema_version\":1,\"operation\":\"get_active_alarm\",\"success\":true,"
          "\"exit_code\":0,\"transport\":{\"http_status\":", stdout);
    print_http(response->http_status);
    fputs("},\"business\":{\"code\":", stdout);
    if (code >= 0) {
        if (doc.tokens[code].type == LJ_STRING) {
            char *value = token_string_alloc(&doc, code);
            json_string(value ? value : "");
            free(value);
        } else fwrite(doc.json + doc.tokens[code].start, 1U,
                      (size_t)(doc.tokens[code].end - doc.tokens[code].start), stdout);
    } else fputs("null", stdout);
    fputs("},\"query\":{\"equip_id\":", stdout);
    fputs(options->history_equip_id, stdout);
    printf("},\"data\":{\"total_alarm_count\":%d,\"returned_alarm_count\":%d,\"alarms\":[",
           total, returned);
    for (int i = 0; i < returned; ++i) {
        int alarm = LjArrayGet(doc.tokens, doc.count, list, i);
        if (i) putchar(',');
        putchar('{');
        int emitted = 0;
        static const char *const names[] = {"seqno", "almid", "almname", "equipid",
            "equiptypeid", "equipname", "level", "reason", "reasonum", "position",
            "localtime", "faultDesc", "locationInfo"};
        for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); ++n)
            print_optional_field(&doc, alarm, names[n], names[n], &emitted);
        CausePair pairs[TOP_CAUSES] = {0};
        int cause_total = 0;
        int count = cause_pairs(&doc, alarm, pairs, &cause_total);
        if (emitted) putchar(',');
        printf("\"cause_repair_total_count\":%d,\"topCauses\":[", cause_total);
        for (int j = 0; j < count; ++j) {
            if (j) putchar(',');
            printf("{\"rank\":%d,\"reason\":", j + 1);
            if (pairs[j].reason[0]) json_string(pairs[j].reason); else fputs("null", stdout);
            fputs(",\"repair\":", stdout);
            if (pairs[j].repair[0]) json_string(pairs[j].repair); else fputs("null", stdout);
            putchar('}');
        }
        printf("],\"cause_repair_truncated\":%s}", cause_total > TOP_CAUSES ? "true" : "false");
    }
    printf("],\"truncated\":%s},\"error\":null}\n",
           total > returned ? "true" : "false");
    doc_close(&doc);
}

static int tokens_equal(const JsonDoc *doc, int left, int right)
{
    if (left < 0 || right < 0) return 0;
    int64_t left_number = 0;
    int64_t right_number = 0;
    if (!token_int(doc, left, &left_number) && !token_int(doc, right, &right_number))
        return left_number == right_number;
    int ln = doc->tokens[left].end - doc->tokens[left].start;
    int rn = doc->tokens[right].end - doc->tokens[right].start;
    if (doc->tokens[left].type != doc->tokens[right].type) return 0;
    return ln == rn && !strncmp(doc->json + doc->tokens[left].start,
                               doc->json + doc->tokens[right].start, (size_t)ln);
}

static char *enum_display(const JsonDoc *doc, int signal)
{
    int value = field(doc, signal, "sigValue");
    int enums = field(doc, signal, "enumList");
    if (value < 0 || enums < 0 || doc->tokens[enums].type != LJ_ARRAY) return NULL;
    for (int i = 0; i < doc->tokens[enums].size; ++i) {
        int item = LjArrayGet(doc->tokens, doc->count, enums, i);
        if (tokens_equal(doc, value, field(doc, item, "enumVal")))
            return token_string_alloc(doc, field(doc, item, "enumName"));
    }
    return NULL;
}

void normalized_print_monitor_info(const Options *options, const Buffer *response, int rc)
{
    if (rc != OK) {
        normalized_print_error("get_monitor_info", rc, response ? response->http_status : 0,
                               "query", "Monitor query failed");
        return;
    }
    JsonDoc doc;
    if (doc_open(&doc, response)) {
        normalized_print_error("get_monitor_info", HTTP_ERROR, response->http_status,
                               "normalize_response", "Invalid monitor response");
        return;
    }
    int list = field(&doc, 0, "sigList");
    int64_t code = -1;
    field_int(&doc, 0, "errCode", &code);
    int count = list >= 0 && doc.tokens[list].type == LJ_ARRAY ? doc.tokens[list].size : 0;
    fputs("{\"schema_version\":1,\"operation\":\"get_monitor_info\",\"success\":true,"
          "\"exit_code\":0,\"transport\":{\"http_status\":", stdout);
    print_http(response->http_status);
    printf("},\"business\":{\"code\":%lld},\"query\":{\"devices\":[{\"equip_id\":%s,"
           "\"equip_type_id\":%s}],\"para3\":%s,\"para4\":%s,"
           "\"requested_signal_ids\":[]},\"data\":{\"devices\":[{\"equip_id\":%s,"
           "\"equip_type_id\":%s,\"signals\":[",
           (long long)code, options->equip_id, options->equip_type_id, options->para3,
           options->para4, options->equip_id, options->equip_type_id);
    for (int i = 0; i < count; ++i) {
        int signal = LjArrayGet(doc.tokens, doc.count, list, i);
        if (i) putchar(',');
        putchar('{');
        int emitted = 0;
        print_optional_field(&doc, signal, "sigId", "sigId", &emitted);
        print_optional_field(&doc, signal, "sigName", "sigName", &emitted);
        print_optional_field(&doc, signal, "sigValue", "sigValue", &emitted);
        int unit = field(&doc, signal, "sigUnit");
        char *unit_value = token_string_alloc(&doc, unit);
        if (unit_value && *unit_value) {
            putchar(',');
            fputs("\"sigUnit\":", stdout);
            json_string(unit_value);
        }
        free(unit_value);
        char *display = enum_display(&doc, signal);
        if (display) {
            fputs(",\"displayValue\":", stdout);
            json_string(display);
            free(display);
        }
        putchar('}');
    }
    printf("]}],\"returned_signal_count\":%d,\"missing_signal_ids\":[],"
           "\"truncated\":false},\"error\":null}\n", count);
    doc_close(&doc);
}

void normalized_print_history_alarm(const Options *options, const Buffer *response, int rc)
{
    if (rc != OK) {
        normalized_print_error("get_history_alarm", rc, response ? response->http_status : 0,
                               "query", "History alarm query failed");
        return;
    }
    JsonDoc doc;
    if (doc_open(&doc, response)) {
        normalized_print_error("get_history_alarm", HTTP_ERROR, response->http_status,
                               "normalize_response", "Invalid history alarm response");
        return;
    }
    int list = field(&doc, 0, "hisalmlist");
    int code = field(&doc, 0, "errcode");
    int total_token = field(&doc, 0, "totalnum");
    int count = list >= 0 && doc.tokens[list].type == LJ_ARRAY ? doc.tokens[list].size : 0;
    int returned = count > NORMALIZED_MAX_ALARMS ? NORMALIZED_MAX_ALARMS : count;
    fputs("{\"schema_version\":1,\"operation\":\"get_history_alarm\",\"success\":true,"
          "\"exit_code\":0,\"transport\":{\"http_status\":", stdout);
    print_http(response->http_status);
    fputs("},\"business\":{\"code\":", stdout);
    int64_t number = 0;
    if (!token_int(&doc, code, &number)) printf("%lld", (long long)number);
    else fputs("null", stdout);
    fputs("},\"query\":{\"equip_id\":", stdout);
    fputs(options->history_equip_id, stdout);
    fputs(",\"start_time\":", stdout); json_string(options->start_time ? options->start_time : "default");
    fputs(",\"end_time\":", stdout); json_string(options->end_time ? options->end_time : "default");
    printf(",\"page_index\":%s,\"page_size\":%s,\"alarm_level\":%s},"
           "\"data\":{\"total_count\":", options->page_index, options->page_size,
           options->alarm_level);
    if (!token_int(&doc, total_token, &number)) printf("%lld", (long long)number);
    else printf("%d", count);
    printf(",\"returned_count\":%d,\"alarms\":[", returned);
    for (int i = 0; i < returned; ++i) {
        int alarm = LjArrayGet(doc.tokens, doc.count, list, i);
        if (i) putchar(',');
        putchar('{');
        int emitted = 0;
        static const struct { const char *source; const char *target; } names[] = {
            {"seqno", "seqno"}, {"alarmed", "alarm_id"}, {"alarmname", "alarm_name"},
            {"equipid", "equipid"}, {"equiptypeid", "equiptypeid"},
            {"equipname", "equipname"}, {"level", "level"},
            {"startime", "start_time"}, {"endtime", "end_time"},
            {"reason", "reason"}, {"reasonum", "reasonum"}};
        for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); ++n)
            print_optional_field(&doc, alarm, names[n].source, names[n].target, &emitted);
        putchar('}');
    }
    printf("],\"truncated\":%s},\"error\":null}\n",
           count > returned ? "true" : "false");
    doc_close(&doc);
}

int normalized_parse_set_result(const Buffer *response, const Options *options,
                                FrequencyBandResult *result)
{
    JsonDoc doc;
    if (doc_open(&doc, response)) return -1;
    int64_t code = -1;
    if (field_int(&doc, 0, "errCode", &code)) {
        doc_close(&doc);
        return -1;
    }
    result->write_business_code = (int)code;
    int list = field(&doc, 0, "resultList");
    int found = 0;
    if (list >= 0 && doc.tokens[list].type == LJ_ARRAY) {
        for (int i = 0; i < doc.tokens[list].size; ++i) {
            int item = LjArrayGet(doc.tokens, doc.count, list, i);
            int64_t equip_id, equip_type_id, device_result;
            if (field_int(&doc, item, "equipId", &equip_id) ||
                field_int(&doc, item, "equipTypeId", &equip_type_id) ||
                equip_id != strtoll(options->equip_id, NULL, 10) ||
                equip_type_id != strtoll(options->equip_type_id, NULL, 10)) continue;
            if (field_int(&doc, item, "result", &device_result)) continue;
            result->device_result = (int)device_result;
            int signals = field(&doc, item, "sigResultList");
            if (signals < 0 || doc.tokens[signals].type != LJ_ARRAY) continue;
            for (int j = 0; j < doc.tokens[signals].size; ++j) {
                int signal = LjArrayGet(doc.tokens, doc.count, signals, j);
                int64_t signal_id, signal_result;
                if (!field_int(&doc, signal, "sigId", &signal_id) &&
                    signal_id == FREQ_BAND_SIG_ID &&
                    !field_int(&doc, signal, "setResult", &signal_result)) {
                    result->signal_result = (int)signal_result;
                    found = 1;
                    break;
                }
            }
        }
    }
    result->write_result_known = found;
    doc_close(&doc);
    return found && code == 0 && result->device_result == 0 && result->signal_result == 0 ? 0 : -1;
}

void normalized_print_frequency_band(const Options *options, const FrequencyBandResult *result, int rc)
{
    static const char *const values[] = {"1", "3", "23", "7", "13"};
    fputs("{\"schema_version\":1,\"operation\":\"set_freq_band\",\"success\":", stdout);
    fputs(rc == OK ? "true" : "false", stdout);
    printf(",\"exit_code\":%d,\"target\":{\"equip_id\":%s,\"equip_type_id\":%s,"
           "\"signal_id\":%d},\"before\":", rc, options->equip_id,
           options->equip_type_id, FREQ_BAND_SIG_ID);
    if (result->before_band >= 0)
        printf("{\"raw_value\":%s,\"band_number\":%d}", values[result->before_band], result->before_band + 1);
    else fputs("null", stdout);
    fputs(",\"planned\":", stdout);
    if (result->target_band >= 0)
        printf("{\"raw_value\":%s,\"band_number\":%d}", values[result->target_band], result->target_band + 1);
    else fputs("null", stdout);
    fputs(",\"write\":", stdout);
    if (!result->write_attempted) fputs("null", stdout);
    else {
        fputs("{\"http_status\":", stdout);
        print_http(result->write_http_status);
        fputs(",\"business_code\":", stdout);
        if (result->write_result_known) printf("%d", result->write_business_code);
        else fputs("null", stdout);
        fputs(",\"device_results\":", stdout);
        if (result->write_result_known)
            printf("[{\"equip_id\":%s,\"equip_type_id\":%s,\"result\":%d,"
                   "\"signal_results\":[{\"sig_id\":%d,\"set_result\":%d}]}]",
                   options->equip_id, options->equip_type_id, result->device_result,
                   FREQ_BAND_SIG_ID, result->signal_result);
        else fputs("[]", stdout);
        printf(",\"accepted\":%s}",
               result->write_result_known && result->write_business_code == 0 &&
                       result->device_result == 0 && result->signal_result == 0 ? "true" : "false");
    }
    fputs(",\"after\":", stdout);
    if (result->after_band >= 0)
        printf("{\"raw_value\":%s,\"band_number\":%d}", values[result->after_band], result->after_band + 1);
    else fputs("null", stdout);
    fputs(",\"verification\":{\"readback_matches\":", stdout);
    if (result->after_band < 0 || result->target_band < 0) fputs("null", stdout);
    else fputs(result->readback_matches ? "true" : "false", stdout);
    fputs(",\"parameter_result\":", stdout);
    json_string(rc == OK ? "success" : result->after_band >= 0 ? "failed" : "unknown");
    fputs("},\"error\":", stdout);
    if (rc == OK) fputs("null", stdout);
    else {
        fputs("{\"kind\":", stdout); json_string(error_kind(rc));
        fputs(",\"stage\":", stdout); json_string(result->failed_stage ? result->failed_stage : "unknown");
        fputs(",\"message\":\"Frequency-band update did not complete successfully\"}", stdout);
    }
    fputs("}\n", stdout);
}
