#include "litecrab/config.h"
#include "litecrab/hub.h"
#include "litecrab/json.h"
#include "litecrab/kernel.h"
#include "litecrab/observability.h"
#include "litecrab/runtime.h"
#include "litecrab/working_memory.h"

#include <limits.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
static char sessionTestRoot[] = "/tmp/litecrab-session-tests-XXXXXX";
#define CHECK(x)                                                         \
    do {                                                                 \
        checks++;                                                        \
        if (!(x)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            failures++;                                                  \
        }                                                                \
    } while (0)

static int count_session_files(void) {
    DIR* directory = opendir(AgentSessionStoreDirectory());
    if (!directory)
        return -1;
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(directory))) {
        size_t length = strlen(entry->d_name);
        if (length > 4 && !strcmp(entry->d_name + length - 4, ".lcs"))
            count++;
    }
    closedir(directory);
    return count;
}
static void call_init(CrabToolCall* c, const char* name) {
    memset(c, 0, sizeof *c);
    snprintf(c->callId, sizeof c->callId, "test-1");
    snprintf(c->toolName, sizeof c->toolName, "%s", name);
}
static void str_arg(CrabToolCall* c, const char* n, const char* v) {
    CrabToolArg* a = &c->args[c->argCount++];
    snprintf(a->name, sizeof a->name, "%s", n);
    a->value.type = CRAB_TOOL_VALUE_STRING;
    a->value.stringValue = v;
}
static void int_arg(CrabToolCall* c, const char* n, int64_t v) {
    CrabToolArg* a = &c->args[c->argCount++];
    snprintf(a->name, sizeof a->name, "%s", n);
    a->value.type = CRAB_TOOL_VALUE_INT;
    a->value.intValue = v;
}
static void string_array_arg(CrabToolCall* c,
                             const char* n,
                             const char* const* values,
                             size_t count) {
    CrabToolArg* a = &c->args[c->argCount++];
    snprintf(a->name, sizeof a->name, "%s", n);
    a->value.type = CRAB_TOOL_VALUE_STRING_ARRAY;
    a->value.stringArrayValue.count = count;
    for (size_t i = 0; i < count && i < CRAB_TOOL_ARRAY_MAX_ITEMS; i++)
        a->value.stringArrayValue.items[i] = values[i];
}

typedef struct {
    int worker;
    int count;
    int failures;
} SessionStressArg;

static void* session_stress_worker(void* value) {
    SessionStressArg* arg = value;
    for (int i = 0; i < arg->count; i++) {
        char id[64], user[64], json[256];
        snprintf(id, sizeof id, "parallel-%02d-%04d", arg->worker, i);
        snprintf(user, sizeof user, "parallel-user-%02d", arg->worker);
        snprintf(json,
                 sizeof json,
                 "[{\"role\":\"user\",\"content\":\"P-%02d-%04d\"}]",
                 arg->worker,
                 i);
        SessionHandle handle = {0};
        if (AgentSessionStateAcquire(id, 1, &handle) || AgentSessionStateUse(&handle) ||
            AgentSessionStateBindUser(user) || AgentSessionStateSave(json))
            arg->failures++;
        AgentSessionStateRelease(&handle);
    }
    return NULL;
}
static void bool_arg(CrabToolCall* c, const char* n, int v) {
    CrabToolArg* a = &c->args[c->argCount++];
    snprintf(a->name, sizeof a->name, "%s", n);
    a->value.type = CRAB_TOOL_VALUE_BOOL;
    a->value.boolValue = v;
}
static int test_path_join(char* out, size_t size, const char* parent, const char* child) {
    int n = snprintf(out, size, "%s/%s", parent, child);
    return n < 0 || (size_t) n >= size ? -1 : 0;
}

static void test_json(void) {
    const char* j = "{\"a\":\"x\\nq\",\"n\":-12,\"b\":true,\"arr\":[1,2],\"u\":\"\\u4f60\\u597d\"}";
    LjToken t[40];
    LjParser p;
    LjInit(&p);
    int n = LjParse(&p, j, strlen(j), t, 40);
    CHECK(n > 0);
    int a = LjObjectGet(j, t, n, 0, "a"), num = LjObjectGet(j, t, n, 0, "n"),
        b = LjObjectGet(j, t, n, 0, "b"), arr = LjObjectGet(j, t, n, 0, "arr"),
        u = LjObjectGet(j, t, n, 0, "u");
    char s[16];
    int64_t iv;
    int bv;
    CHECK(a > 0 && !LjString(j, &t[a], s, sizeof s) && !strcmp(s, "x\nq"));
    CHECK(num > 0 && !LjInt64(j, &t[num], &iv) && iv == -12);
    CHECK(b > 0 && !LjBool(j, &t[b], &bv) && bv == 1);
    CHECK(LjArrayGet(t, n, arr, 1) > 0);
    CHECK(u > 0 && !LjString(j, &t[u], s, sizeof s) && !strcmp(s, "你好"));
    CHECK(!LjValidate(j, LJ_OBJECT));
    CHECK(LjValidate("{bad", LJ_OBJECT) != 0);
    CHECK(LjValidate("{\"x\":bad}", LJ_OBJECT) != 0);
    CHECK(LjValidate("{\"x\" \"y\"}", LJ_OBJECT) != 0);
}

static void test_runtime(void) {
    char temp[] = "/tmp/litecrab-test-XXXXXX";
    char* root = mkdtemp(temp);
    CHECK(root != NULL);
    CrabRuntime rt;
    CrabRuntimeConfig cfg = {.workspaceRoot = root};
    CHECK(!CrabRuntimeInit(&rt, &cfg));
    CHECK(!CrabRuntimeRegisterBuiltinTools(&rt));
    CHECK(rt.registry.toolCount == 12);
    CrabToolCatalogView cat;
    CHECK(!CrabRuntimeGetToolCatalog(&rt, &cat));
    char schema[65536];
    CHECK(!LlmToolAdapterRenderOpenaiToolsJson(&cat, schema, sizeof schema));
    CHECK(!LjValidate(schema, LJ_ARRAY));
    CHECK(strstr(schema, "\"additionalProperties\":false") != NULL);
    CHECK(strstr(schema, "Never use this for SKILL.md or files under a skill's references") &&
          strstr(schema, "skillPath and the relative path as fileName") &&
          strstr(schema, "references/通信异常.md"));
    char baseSchema[65536];
    CHECK(!LlmToolAdapterRenderOpenaiToolsJsonFiltered(&cat, 0, baseSchema, sizeof baseSchema));
    CHECK(!strstr(baseSchema, "\"name\":\"skill_read\"") &&
          !strstr(baseSchema, "\"name\":\"skill_complete\"") &&
          strstr(baseSchema, "\"name\":\"read\""));
    CrabToolCall c;
    CrabToolResponse r;
    call_init(&c, "write");
    str_arg(&c, "path", "a.txt");
    str_arg(&c, "content", "alpha\nbeta\nalpha\n");
    str_arg(&c, "mode", "create");
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    CHECK(r.success && strstr(r.content, "exit_code: 0"));
    CHECK(strstr(schema, "\"name\":\"shell\"") == NULL);
    CHECK(strstr(schema, "\"name\":\"exec_program\"") != NULL);
    CHECK(strstr(schema, "\"name\":\"ls\"") != NULL);
    CHECK(strstr(schema, "\"name\":\"pwd\"") != NULL);
    call_init(&c, "shell");
    str_arg(&c, "script", "printf shell-ok");
    CHECK(CrabRuntimeCallTool(&rt, &c, &r) == CRAB_ERROR_TOOL_DISABLED);
    CHECK(!r.success && strstr(r.summary, "tool is disabled"));

    char executable[512];
    CHECK(!test_path_join(executable, sizeof executable, root, "argv-probe"));
    FILE* probe = fopen(executable, "w");
    CHECK(probe != NULL);
    if (probe) {
        fputs("#!/bin/sh\nprintf '<%s>|<%s>' \"$1\" \"$2\"\n", probe);
        fclose(probe);
    }
    CHECK(!chmod(executable, 0700));
    const char* literalArgs[] = {"argument with spaces", "x;printf-not-executed"};
    call_init(&c, "exec_program");
    str_arg(&c, "command", "argv-probe");
    str_arg(&c, "path", ".");
    string_array_arg(&c, "args", literalArgs, 2);
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    CHECK(r.success && r.toolExitCode == 0 &&
          strstr(r.content, "<argument with spaces>|<x;printf-not-executed>"));

    char floodExecutable[512];
    CHECK(!test_path_join(floodExecutable, sizeof floodExecutable, root, "output-flood"));
    FILE* floodProbe = fopen(floodExecutable, "w");
    CHECK(floodProbe != NULL);
    if (floodProbe) {
        fputs("#!/bin/sh\nwhile :; do printf '0123456789abcdef0123456789abcdef\\n'; done\n",
              floodProbe);
        fclose(floodProbe);
    }
    CHECK(!chmod(floodExecutable, 0700));
    call_init(&c, "exec_program");
    str_arg(&c, "command", "output-flood");
    str_arg(&c, "path", ".");
    string_array_arg(&c, "args", NULL, 0);
    int_arg(&c, "timeoutMs", 5000);
    CHECK(CrabRuntimeCallTool(&rt, &c, &r) != 0);
    CHECK(!r.success);
    CHECK(r.toolExitCode == 125);
    CHECK(r.truncated);

    char timeoutExecutable[512];
    CHECK(!test_path_join(timeoutExecutable, sizeof timeoutExecutable, root, "ignore-term"));
    FILE* timeoutProbe = fopen(timeoutExecutable, "w");
    CHECK(timeoutProbe != NULL);
    if (timeoutProbe) {
        fputs("#!/bin/sh\ntrap '' TERM\nsleep 10\n", timeoutProbe);
        fclose(timeoutProbe);
    }
    CHECK(!chmod(timeoutExecutable, 0700));
    call_init(&c, "exec_program");
    str_arg(&c, "command", "ignore-term");
    str_arg(&c, "path", ".");
    string_array_arg(&c, "args", NULL, 0);
    int_arg(&c, "timeoutMs", 100);
    int64_t timeoutStarted = LiteMonotonicMs();
    CHECK(CrabRuntimeCallTool(&rt, &c, &r) != 0);
    CHECK(!r.success);
    CHECK(r.toolExitCode == 124);
    CHECK(r.truncated);
    CHECK(LiteMonotonicMs() - timeoutStarted < 1500);

    char parseError[256];
    CHECK(!LlmToolAdapterParseCall(&cat,
                                   "exec-parse",
                                   "exec_program",
                                   "{\"command\":\"argv-probe\",\"path\":\".\",\"args\":[\"a b\",\"x;y\"]}",
                                   &c,
                                   parseError,
                                   sizeof parseError));
    const CrabToolStringArray* parsedArgs = CrabToolArgsGetStringArray(&c, "args");
    CHECK(parsedArgs && parsedArgs->count == 2 && !strcmp(parsedArgs->items[0], "a b") &&
          !strcmp(parsedArgs->items[1], "x;y"));
    LlmToolAdapterFreeCall(&c);
    call_init(&c, "read");
    str_arg(&c, "path", "a.txt");
    int_arg(&c, "startLine", 2);
    int_arg(&c, "maxLines", 1);
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    CHECK(strstr(r.content, "2: beta") != NULL);
    call_init(&c, "edit");
    str_arg(&c, "path", "a.txt");
    str_arg(&c, "oldText", "alpha");
    str_arg(&c, "newText", "gamma");
    int_arg(&c, "expectedOccurrences", 1);
    CHECK(CrabRuntimeCallTool(&rt, &c, &r) != 0);
    CHECK(!r.success && strstr(r.summary, "expected 1 occurrences, found 2"));
    c.args[3].value.intValue = 2;
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    CHECK(r.success);
    call_init(&c, "grep");
    str_arg(&c, "pattern", "GAMMA");
    str_arg(&c, "path", ".");
    bool_arg(&c, "caseSensitive", 0);
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    CHECK(strstr(r.content, "a.txt:1:gamma") != NULL);
    call_init(&c, "grep");
    str_arg(&c, "pattern", "definitely-not-present");
    str_arg(&c, "path", ".");
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    CHECK(r.success && r.toolExitCode == 0 && strstr(r.content, "exit_code: 0"));
    CHECK(!strstr(r.content, "no matches"));
    char grepLargePath[512];
    snprintf(grepLargePath, sizeof grepLargePath, "%s/grep-large.txt", root);
    FILE* grepLarge = fopen(grepLargePath, "w");
    CHECK(grepLarge != NULL);
    if (grepLarge) {
        for (int i = 0; i < 20; i++)
            fprintf(grepLarge,
                    "needle-%02d-%01000d\n",
                    i,
                    i);
        fclose(grepLarge);
    }
    call_init(&c, "grep");
    str_arg(&c, "pattern", "needle");
    str_arg(&c, "path", "grep-large.txt");
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    CHECK(r.success && r.toolExitCode == 0 && strstr(r.content, "needle-19"));
    call_init(&c, "glob");
    str_arg(&c, "pattern", "*.txt");
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    CHECK(strstr(r.content, "a.txt") != NULL);
    call_init(&c, "pwd");
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    CHECK(r.success && strstr(r.content, root) != NULL);
    call_init(&c, "ls");
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    CHECK(r.success && strstr(r.content, "file\ta.txt") != NULL);
    call_init(&c, "ls");
    str_arg(&c, "path", "../");
    CHECK(CrabRuntimeCallTool(&rt, &c, &r) == CRAB_ERROR_INVALID_ARG);
    call_init(&c, "write");
    str_arg(&c, "path", "data.csv");
    str_arg(&c, "content", "name,note\nAlice,\"a,b\"\n");
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    call_init(&c, "csv_read");
    str_arg(&c, "path", "data.csv");
    int_arg(&c, "row", 2);
    str_arg(&c, "columnName", "note");
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    CHECK(strstr(r.content, "value: a,b") != NULL);
    call_init(&c, "read");
    str_arg(&c, "path", "../etc/passwd");
    CHECK(CrabRuntimeCallTool(&rt, &c, &r) == CRAB_ERROR_INVALID_ARG);
    CHECK(r.runtimeError == 0 &&
          r.toolExitCode == CRAB_ERROR_INVALID_ARG); /* execute-level path rejection */
    call_init(&c, "read");
    str_arg(&c, "path", "a.txt");
    str_arg(&c, "path", "a.txt");
    char err[128];
    CHECK(CrabToolValidateCall(CrabRegistryFind(&rt.registry, "read"), &c, err, sizeof err) ==
          CRAB_ERROR_INVALID_ARG);
    CHECK(strstr(err, "duplicate"));
    CrabToolCall parsed;
    CHECK(!LlmToolAdapterParseCall(
        &cat, "c1", "write", "{\"path\":\"z.txt\",\"createDirs\":true}", &parsed, err, sizeof err));
    CHECK(parsed.argCount == 2 && parsed.args[1].value.boolValue);
    LlmToolAdapterFreeCall(&parsed);
    CHECK(!LlmToolAdapterParseCall(&cat, "c2", "pwd", "{}", &parsed, err, sizeof err));
    CHECK(parsed.argCount == 0);
    LlmToolAdapterFreeCall(&parsed);
    CHECK(LlmToolAdapterParseCall(&cat, "c1", "write", "{\"bogus\":1}", &parsed, err, sizeof err) !=
          0);
    unlink("/tmp/no-such-litecrab-file");
    char p[512];
    snprintf(p, sizeof p, "%s/a.txt", root);
    unlink(p);
    snprintf(p, sizeof p, "%s/data.csv", root);
    unlink(p);
    unlink(executable);
    unlink(floodExecutable);
    unlink(timeoutExecutable);
    rmdir(root);
    CrabRuntimeDestroy(&rt);
}

static void test_hub(void) {
    MessageBusDestroy();
    MessageBusInit();
    LiteMsg a = {0}, b = {0};
    snprintf(a.requestId, sizeof a.requestId, "a");
    a.content = strdup("A");
    snprintf(b.requestId, sizeof b.requestId, "b");
    b.content = strdup("B");
    CHECK(!MessageBusPushOutbound(&a));
    CHECK(!MessageBusPushOutbound(&b));
    LiteMsg out = {0};
    CHECK(!MessageBusPopOutboundByRequestId("b", &out));
    CHECK(out.content && !strcmp(out.content, "B"));
    LiteMsgClear(&out);
    CHECK(!MessageBusPopOutboundByRequestId("a", &out));
    LiteMsgClear(&out);
    CHECK(MessageBusTimedPopOutboundByRequestId("missing", &out, 20) != 0);
    IngressResult r1, r2;
    CHECK(!IngressSubmit("network:tcp", "hello", 5, NULL, &r1));
    CHECK(!IngressSubmit("ipc:local", "world", 5, NULL, &r2));
    CHECK(strcmp(r1.requestId, r2.requestId));
    CHECK(!MessageBusPopInbound(&out));
    CHECK(!strcmp(out.channel, "network") && !strcmp(out.userId, "anonymous"));
    LiteMsgClear(&out);
    CHECK(!MessageBusPopInbound(&out));
    LiteMsgClear(&out);
    IngressOptions routed = {.source = "network:tcp",
                             .userId = "u",
                             .sessionId = "s",
                             .type = LITE_MSG_CHAT,
                             .priority = LITE_PRIORITY_NORMAL,
                             .replyMode = LITE_REPLY_SYNC,
                             .replyToRunId = "run-1",
                             .replyToInterruptId = "int-1",
                             .correlationToken = "token-1"};
    IngressResult routedResult;
    CHECK(!IngressSubmit("network:tcp", "resume", 6, &routed, &routedResult));
    CHECK(!MessageBusPopInbound(&out));
    CHECK(!strcmp(out.replyToRunId, "run-1") && !strcmp(out.replyToInterruptId, "int-1") &&
          !strcmp(out.correlationToken, "token-1"));
    CHECK(!RequestShouldStop(&out));
    char routedRequestId[LITE_MAX_REQUEST_ID_LEN];
    snprintf(routedRequestId, sizeof routedRequestId, "%s", out.requestId);
    LiteMsgClear(&out);
    char responseText[64];
    CHECK(DispatchResponse(routedRequestId,
                           LITE_REPLY_SYNC,
                           20,
                           responseText,
                           sizeof responseText) != 0);
    LiteMsg late = {.replyMode = LITE_REPLY_SYNC, .content = strdup("too late")};
    snprintf(late.requestId, sizeof late.requestId, "%s", routedRequestId);
    CHECK(RequestComplete(&late) != 0);
    LiteMsgClear(&late);
    /* A timeout must release its registry slot. Repeating beyond registry
     * capacity catches leaked/orphaned slots without blocking the test. */
    for (int i = 0; i < 40; i++) {
        IngressResult timed;
        CHECK(!IngressSubmit("network:tcp", "timeout", 7, &routed, &timed));
        CHECK(!MessageBusPopInbound(&out));
        LiteMsgClear(&out);
        CHECK(DispatchResponse(
                  timed.requestId, LITE_REPLY_SYNC, 0, responseText, sizeof responseText) != 0);
    }
    IngressResult classified;
    CHECK(!IngressSubmit("network:tcp", "classified", 10, &routed, &classified));
    CHECK(!MessageBusPopInbound(&out));
    LiteMsg classifiedResponse = {.replyMode = LITE_REPLY_SYNC,
                                  .completionStatus = LITE_COMPLETION_RETRYABLE,
                                  .content = strdup("temporary failure")};
    snprintf(classifiedResponse.requestId,
             sizeof classifiedResponse.requestId,
             "%s",
             classified.requestId);
    CHECK(!RequestComplete(&classifiedResponse));
    LiteCompletionStatus completion = LITE_COMPLETION_HANDLED;
    CHECK(!DispatchResponseEx(classified.requestId,
                              LITE_REPLY_SYNC,
                              20,
                              responseText,
                              sizeof responseText,
                              &completion));
    CHECK(completion == LITE_COMPLETION_RETRYABLE && !strcmp(responseText, "temporary failure"));
    LiteMsgClear(&out);
    IngressResult cancelFirst, cancelSecond;
    CHECK(!IngressSubmit("network:tcp", "cancel-1", 8, &routed, &cancelFirst));
    CHECK(!IngressSubmit("network:tcp", "cancel-2", 8, &routed, &cancelSecond));
    CHECK(RequestCancelAll() == 2);
    CHECK(!MessageBusPopInbound(&out));
    CHECK(RequestShouldStop(&out));
    LiteMsgClear(&out);
    CHECK(!MessageBusPopInbound(&out));
    CHECK(RequestShouldStop(&out));
    LiteMsgClear(&out);
    CHECK(DispatchResponse(cancelFirst.requestId,
                           LITE_REPLY_SYNC,
                           0,
                           responseText,
                           sizeof responseText) != 0);
    char* huge = malloc(INGRESS_MAX_REQ_BYTES + 2);
    if (!huge) {
        CHECK(huge != NULL);
        MessageBusDestroy();
        return;
    }
    memset(huge, 'x', INGRESS_MAX_REQ_BYTES + 1);
    CHECK(IngressSubmit("x", huge, INGRESS_MAX_REQ_BYTES + 1, NULL, &r1) != 0 &&
          r1.status == INGRESS_REJECTED);
    free(huge);
    MessageBusDestroy();
}

static void test_session_skill(void) {
    CHECK(mkdtemp(sessionTestRoot) != NULL);
    CHECK(!AgentSessionStoreConfigure(sessionTestRoot));
    AgentSessionStateInit();
    CHECK(!AgentSessionStateOpen());
    CHECK(AgentSessionStateOpen() != 0);
    AgentSessionStateSetSessionId("s1");
    AgentSessionStateBindUser("u1");
    char* json = malloc(60000);
    if (!json) {
        CHECK(json != NULL);
        AgentSessionStateClose();
        return;
    }
    strcpy(json, "[");
    for (int i = 0; i < 40; i++) {
        char b[128];
        snprintf(b, sizeof b, "%s{\"role\":\"user\",\"content\":\"m%d\"}", i ? "," : "", i);
        strcat(json, b);
    }
    strcat(json, "]");
    CHECK(!AgentSessionStateSave(json));
    CHECK(!LjValidate(AgentSessionStateLoad(), LJ_ARRAY));
    LjToken t[512];
    LjParser p;
    LjInit(&p);
    int n = LjParse(&p, AgentSessionStateLoad(), strlen(AgentSessionStateLoad()), t, 512);
    CHECK(n > 0 && t[0].size == 32);
    memset(json, 'a', 50000);
    json[50000] = 0;
    char* big = malloc(51000);
    snprintf(big, 51000, "[{\"role\":\"user\",\"content\":\"%s\"}]", json);
    CHECK(!AgentSessionStateSave(big));
    CHECK(!LjValidate(AgentSessionStateLoad(), LJ_ARRAY));
    CHECK(!strcmp(AgentSessionStateLoad(), "[]"));
    free(big);
    free(json);
    AgentSessionStateClose();
    CHECK(!AgentSessionStateOpen());
    CHECK(!SkillSupersetActivate("one", "one/scripts"));
    int step;
    SkillSupersetIncrementStep(&step);
    CHECK(step == 1);
    const SkillStateSuperset* s = SkillSupersetGetActive();
    CHECK(s && s->exec.active);
    CHECK(!SkillSuspendQueuePush(s));
    SkillSupersetFinish(SKILL_EXEC_PHASE_PARTIAL, "suspended");
    CHECK(!SkillSupersetActivate("two", "two/scripts"));
    SkillSupersetIncrementStep(&step);
    CHECK(!SkillSuspendQueuePush(SkillSupersetGetActive()));
    SkillSupersetFinish(SKILL_EXEC_PHASE_PARTIAL, "suspended");
    char ctx[512];
    CHECK(!SkillSuspendQueueBuildSimpleContext(ctx, sizeof ctx));
    CHECK(strstr(ctx, "two") < strstr(ctx, "one"));
    SkillSuspendEntry e;
    CHECK(!SkillSuspendQueuePopByName("one", &e));
    CHECK(!SkillSupersetReactivate(&e));
    CHECK(SkillSupersetGetActive()->exec.phase == SKILL_EXEC_PHASE_RESUMING &&
          SkillSupersetGetActive()->exec.stepCount == 1);
    SkillSupersetOnSessionClose();
    SkillSuspendQueueClear();
    AgentSessionStateClose();
}

static void test_session_isolation(void) {
    AgentSessionStateInit();
    SkillSupersetReset();
    SkillSuspendQueueClear();
    WorkingMemoryInit();

    CHECK(!AgentSessionStateSelect("isolation-a", 1));
    CHECK(!AgentSessionStateBindUser("same-user"));
    CHECK(!AgentSessionStateSave("[{\"role\":\"user\",\"content\":\"A_ONLY\"}]"));
    WorkingMemorySetGoal("GOAL_A");
    CHECK(!SkillSupersetActivate("skill-a", "a/scripts"));
    CHECK(SkillSupersetGetActive() &&
          !strcmp(SkillSupersetGetActive()->basic.skillName, "skill-a"));

    CHECK(!AgentSessionStateSelect("isolation-b", 1));
    CHECK(!AgentSessionStateBindUser("same-user"));
    CHECK(!strcmp(AgentSessionStateLoad(), "[]"));
    CHECK(!WorkingMemoryGet()->goal[0]);
    CHECK(SkillSupersetGetActive() == NULL);
    CHECK(!AgentSessionStateSave("[{\"role\":\"user\",\"content\":\"B_ONLY\"}]"));
    WorkingMemorySetGoal("GOAL_B");
    CHECK(!SkillSupersetActivate("skill-b", "b/scripts"));

    CHECK(!AgentSessionStateSelect("isolation-a", 0));
    CHECK(strstr(AgentSessionStateLoad(), "A_ONLY") && !strstr(AgentSessionStateLoad(), "B_ONLY"));
    CHECK(!strcmp(WorkingMemoryGet()->goal, "GOAL_A"));
    CHECK(SkillSupersetGetActive() &&
          !strcmp(SkillSupersetGetActive()->basic.skillName, "skill-a"));
    char runA[64];
    snprintf(runA, sizeof runA, "%s", SkillSupersetGetActive()->basic.skillRunId);
    CHECK(!SkillSuspendQueuePush(SkillSupersetGetActive()));
    CHECK(!SkillSupersetFinish(SKILL_EXEC_PHASE_PARTIAL, "wait-a"));

    CHECK(!AgentSessionStateSelect("isolation-b", 0));
    CHECK(strstr(AgentSessionStateLoad(), "B_ONLY") && !strstr(AgentSessionStateLoad(), "A_ONLY"));
    CHECK(!strcmp(WorkingMemoryGet()->goal, "GOAL_B"));
    CHECK(SkillSupersetGetActive() &&
          !strcmp(SkillSupersetGetActive()->basic.skillName, "skill-b"));
    char runB[64];
    snprintf(runB, sizeof runB, "%s", SkillSupersetGetActive()->basic.skillRunId);
    CHECK(!SkillSuspendQueuePush(SkillSupersetGetActive()));
    CHECK(!SkillSupersetFinish(SKILL_EXEC_PHASE_PARTIAL, "wait-b"));
    CHECK(SkillSuspendQueueGetCount() == 1);

    AgentSessionStateCloseById("isolation-a");
    SkillRunSnapshot snap;
    CHECK(!SkillRunGetSnapshot("isolation-a", runA, &snap) && snap.status == SKILL_RUN_CANCELLED);
    CHECK(!AgentSessionStateSelect("isolation-b", 0));
    CHECK(strstr(AgentSessionStateLoad(), "B_ONLY"));
    CHECK(!strcmp(WorkingMemoryGet()->goal, "GOAL_B"));
    CHECK(SkillSuspendQueueGetCount() == 1);
    CHECK(!SkillRunGetSnapshot("isolation-b", runB, &snap) &&
          snap.status == SKILL_RUN_WAITING_INPUT);

    CHECK(AgentSessionStateSelect("missing-session", 0) == -1);
    CHECK(!strcmp(AgentSessionStateGetSessionId(), "isolation-b"));

    CHECK(!SkillRouterRecover("skill-b", runB, NULL, "isolation-b", "resume-b"));
    CHECK(SkillSupersetGetActive() != NULL);
    CHECK(AgentSessionStateBindUser("different-user") == -2);
    CHECK(strstr(AgentSessionStateLoad(), "B_ONLY"));
    CHECK(!strcmp(WorkingMemoryGet()->goal, "GOAL_B"));
    CHECK(SkillSupersetGetActive() != NULL);
    CHECK(!SkillRunGetSnapshot("isolation-b", runB, &snap) &&
          snap.status == SKILL_RUN_RUNNING);

    AgentSessionStateSetSkillRoute("skill-b", 1);
    CHECK(AgentSessionStateSkillAllowed("skill-b"));
    CHECK(!AgentSessionStateSkillAllowed("skill-a"));
    AgentSessionStateSetSkillRoute("", 0);
    CHECK(!AgentSessionStateSkillAllowed("skill-a") && !AgentSessionStateSkillAllowed("skill-b"));
    AgentSessionStateCloseById("isolation-b");

    AgentSessionStateInit();
    char capacityId[64];
    for (int i = 0; i < 32; i++) {
        snprintf(capacityId, sizeof capacityId, "capacity-%02d", i);
        CHECK(!AgentSessionStateSelect(capacityId, 1));
    }
    CHECK(!AgentSessionStateSelect("capacity-overflow", 1));
    CHECK(!strcmp(AgentSessionStateGetSessionId(), "capacity-overflow"));
    CHECK(!AgentSessionStateSelect("capacity-00", 0));
    CHECK(!strcmp(AgentSessionStateGetSessionId(), "capacity-00"));
    AgentSessionStateCloseById("capacity-00");
    CHECK(!AgentSessionStateSelect("capacity-reused", 1));
    CHECK(!strcmp(AgentSessionStateGetSessionId(), "capacity-reused"));
    for (int i = 1; i < 32; i++) {
        snprintf(capacityId, sizeof capacityId, "capacity-%02d", i);
        AgentSessionStateCloseById(capacityId);
    }
    AgentSessionStateCloseById("capacity-reused");
}

static void test_session_persistence_and_scale(void) {
    AgentSessionStateInit();
    CHECK(AgentSessionIdValidate("safe-session_1:device.2") == 0);
    CHECK(AgentSessionIdValidate("../escape") != 0);
    CHECK(AgentSessionIdValidate("slash/bad") != 0);
    CHECK(AgentSessionIdValidate("") != 0);

    SessionHandle handle = {0};
    CHECK(!AgentSessionStateAcquire("durable-session", 1, &handle));
    CHECK(!AgentSessionStateUse(&handle));
    CHECK(!AgentSessionStateBindUser("durable-user"));
    CHECK(!AgentSessionStateSave("[{\"role\":\"user\",\"content\":\"DURABLE_VALUE\"}]"));
    WorkingMemorySetGoal("DURABLE_GOAL");
    SessionMetadata metadata;
    CHECK(!AgentSessionStateGetMetadata(&handle, &metadata));
    CHECK(metadata.revision >= 3 && !strcmp(metadata.userId, "durable-user"));
    AgentSessionStateRelease(&handle);

    AgentSessionStateInit();
    CHECK(!AgentSessionStateAcquire("durable-session", 0, &handle));
    CHECK(!AgentSessionStateUse(&handle));
    CHECK(strstr(AgentSessionStateLoad(), "DURABLE_VALUE"));
    CHECK(!strcmp(WorkingMemoryGet()->goal, "DURABLE_GOAL"));
    CHECK(!strcmp(AgentSessionStateGetUserId(), "durable-user"));
    AgentSessionStateRelease(&handle);

    char id[64], message[256];
    for (int i = 0; i < 512; i++) {
        snprintf(id, sizeof id, "bulk-%04d", i);
        CHECK(!AgentSessionStateAcquire(id, 1, &handle));
        CHECK(!AgentSessionStateUse(&handle));
        CHECK(!AgentSessionStateBindUser("bulk-user"));
        snprintf(message,
                 sizeof message,
                 "[{\"role\":\"user\",\"content\":\"VALUE-%04d\"}]",
                 i);
        CHECK(!AgentSessionStateSave(message));
        AgentSessionStateRelease(&handle);
    }
    CHECK(count_session_files() <= 128);
    CHECK(AgentSessionStateAcquire("bulk-0000", 0, &handle) != 0);
    CHECK(!AgentSessionStateAcquire("post-quota", 1, &handle));
    CHECK(!AgentSessionStateUse(&handle));
    CHECK(!AgentSessionStateBindUser("bulk-user"));
    CHECK(!AgentSessionStateSave("[{\"role\":\"user\",\"content\":\"POST_QUOTA\"}]"));
    AgentSessionStateRelease(&handle);

    enum { STRESS_WORKERS = 8, STRESS_SESSIONS_PER_WORKER = 64 };
    pthread_t threads[STRESS_WORKERS];
    SessionStressArg args[STRESS_WORKERS];
    for (int i = 0; i < STRESS_WORKERS; i++) {
        args[i] = (SessionStressArg){.worker = i, .count = STRESS_SESSIONS_PER_WORKER};
        CHECK(!pthread_create(&threads[i], NULL, session_stress_worker, &args[i]));
    }
    for (int i = 0; i < STRESS_WORKERS; i++) {
        CHECK(!pthread_join(threads[i], NULL));
        CHECK(args[i].failures == 0);
    }
    CHECK(count_session_files() <= 128);

    char corruptPath[PATH_MAX];
    snprintf(corruptPath,
             sizeof corruptPath,
             "%s/corrupt-session.lcs",
             AgentSessionStoreDirectory());
    FILE* corrupt = fopen(corruptPath, "wb");
    CHECK(corrupt != NULL);
    if (corrupt) {
        CHECK(fwrite("broken", 1, 6, corrupt) == 6);
        fclose(corrupt);
    }
    AgentSessionStateInit();
    CHECK(AgentSessionStateAcquire("corrupt-session", 0, &handle) != 0);
    unlink(corruptPath);
    AgentSessionStateCloseById("durable-session");
}

static void test_llm_parse(void) {
    LlmConfig c = {.baseUrl = "http://localhost:9999/v1/chat/completions",
                   .apiKey = "k",
                   .model = "m",
                   .maxTokens = 10,
                   .temperature = .1,
                   .stream = 0};
    CHECK(!LlmInit(&c));
    char req[4096];
    CHECK(!LlmBuildChatToolsRequestJson(
        "[{\"role\":\"user\",\"content\":\"hi\"}]", "[]", 0, req, sizeof req));
    CHECK(!LjValidate(req, LJ_OBJECT));
    CHECK(strstr(req, "\"tools\"") == NULL);
    CHECK(strstr(req, "\"tool_choice\"") == NULL);
    CHECK(!LlmBuildChatToolsRequestJson(
        "[{\"role\":\"user\",\"content\":\"hi\"}]", "[ \r\n\t]", 0, req, sizeof req));
    CHECK(strstr(req, "\"tools\"") == NULL);
    CHECK(strstr(req, "\"tool_choice\"") == NULL);
    CHECK(!LlmBuildChatToolsRequestJson(
        "[{\"role\":\"user\",\"content\":\"hi\"}]",
        "[{\"type\":\"function\",\"function\":{\"name\":\"read\"}}]",
        0,
        req,
        sizeof req));
    CHECK(strstr(req, "\"tools\":[") != NULL);
    CHECK(strstr(req, "\"tool_choice\":\"auto\"") != NULL);
    CHECK(LlmBuildChatToolsRequestJson(
              "[{\"role\":\"user\",\"content\":\"hi\"}]", "not-json", 0, req, sizeof req) !=
          0);
    LlmResponse r = {0};
    const char* body =
        "{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":[{\"id\":\"c1\",\"type\":"
        "\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"a\\\"}\"}}]"
        "}}],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":3,\"total_tokens\":5}}";
    CHECK(!LlmParseResponseBody(body, 0, &r));
    CHECK(r.toolUse && r.callCount == 1 && !strcmp(r.calls[0].name, "read") && r.totalTokens == 5);
    CHECK(!strcmp(r.calls[0].input, "{\"path\":\"a\"}"));
    char toolCallsJson[1024];
    CHECK(!SkillHandlerBuildToolCallsJson(&r, toolCallsJson, sizeof toolCallsJson));
    CHECK(!LjValidate(toolCallsJson, LJ_ARRAY));
    CHECK(strstr(toolCallsJson, "\"name\":\"read\"") != NULL);
    CHECK(strstr(toolCallsJson, "\\\"path\\\":\\\"a\\\"") != NULL);
    LlmResponseClear(&r);
    const char* sse =
        "data: "
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c2\",\"function\":{"
        "\"name\":\"write\",\"arguments\":\"{\\\"path\\\":\"}}]}}]}\n\ndata: "
        "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":"
        "\"\\\"x\\\"}\"}}]}}]}\n\ndata: [DONE]\n\n";
    CHECK(!LlmParseResponseBody(sse, 1, &r));
    CHECK(r.toolUse && !strcmp(r.calls[0].input, "{\"path\":\"x\"}"));
    LlmResponseClear(&r);
    const char* sseText = "data: {\"choices\":[{\"delta\":{\"content\":\"你\"}}]}\n\ndata: "
                          "{\"choices\":[{\"delta\":{\"content\":\"好\"}}]}\n\ndata: [DONE]\n";
    CHECK(!LlmParseResponseBody(sseText, 1, &r));
    CHECK(!strcmp(r.text, "你好"));
    LlmResponseClear(&r);
    const char* truncatedSse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n";
    CHECK(LlmParseResponseBody(truncatedSse, 1, &r) != 0);
    CHECK(r.textLen == 0 && r.callCount == 0);
    const char* malformedAfterValid =
        "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n"
        "data: {not-json}\n\ndata: [DONE]\n\n";
    CHECK(LlmParseResponseBody(malformedAfterValid, 1, &r) != 0);
    CHECK(r.textLen == 0 && r.callCount == 0);
    /* reasoning_content only (no content, no tool_calls): must not be treated
     * as a final answer. reasoningTokens should be non-zero so callers can
     * detect the "thinking without output" case. */
    const char* sseReasoning =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"正在分析\"}}]}\n\ndata: "
        "{\"choices\":[{\"delta\":{\"reasoning_content\":\"告警原因\"}}]}\n\ndata: [DONE]\n";
    CHECK(!LlmParseResponseBody(sseReasoning, 1, &r));
    CHECK(r.textLen == 0 && !r.toolUse && r.reasoningTokens > 0);
    LlmResponseClear(&r);
}

static void test_llm_trace_error_code(void) {
    char directory[] = "/tmp/litecrab-trace-tests-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    CHECK(!AgentTraceInit(directory));
    AgentTraceScope scope = {0};
    CHECK(!AgentTraceStart(&scope, "trace-user", "trace-session", "trace-input"));
    AgentTraceLogLlm(&scope,
                     "trace-model",
                     1,
                     "error",
                     0,
                     "[]",
                     "[{\"id\":\"call-1\",\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"a\\\"}\"}}]",
                     "system",
                     "[]",
                     "LLM request failed",
                     0,
                     0,
                     0,
                     -208);
    char tracePath[PATH_MAX];
    snprintf(tracePath, sizeof tracePath, "%s", AgentTraceGetFilename());
    AgentTraceClose();

    FILE* trace = fopen(tracePath, "rb");
    CHECK(trace != NULL);
    char content[8192] = "";
    if (trace) {
        size_t length = fread(content, 1, sizeof content - 1, trace);
        content[length] = 0;
        fclose(trace);
    }
    CHECK(strstr(content, "\"status\":\"error\"") != NULL);
    CHECK(strstr(content, "\"errorCode\":-208") != NULL);
    CHECK(strstr(content, "\"toolCalls\":[{\"id\":\"call-1\"") != NULL);
    unlink(tracePath);
    rmdir(directory);
}

static void test_config_working_memory(void) {
    char base[] = "/tmp/litecrab-base-XXXXXX", llm[] = "/tmp/litecrab-llm-XXXXXX";
    int bfd = mkstemp(base), lfd = mkstemp(llm);
    CHECK(bfd >= 0 && lfd >= 0);
    if (bfd < 0 || lfd < 0) {
        if (bfd >= 0) {
            close(bfd);
            unlink(base);
        }
        if (lfd >= 0) {
            close(lfd);
            unlink(llm);
        }
        return;
    }
    FILE *bf = fdopen(bfd, "w"), *lf = fdopen(lfd, "w");
    CHECK(bf && lf);
    if (!bf || !lf) {
        if (bf)
            fclose(bf);
        else
            close(bfd);
        if (lf)
            fclose(lf);
        else
            close(lfd);
        unlink(base);
        unlink(llm);
        return;
    }
    fputs("{\"server\":{\"listen_ip\":\"127.0.0.1\",\"listen_port\":12345,\"max_req_bytes\":4096},"
          "\"workspace_root\":\"/tmp/work\"}",
          bf);
    fputs("{\"llm\":{\"default\":\"local\",\"local\":{\"baseUrl\":\"http://localhost:9999/v1/chat/"
          "completions\",\"apiKey\":\"test-only\",\"modelName\":\"mock\"},\"max_tokens\":321,"
          "\"stream\":false}}",
          lf);
    fclose(bf);
    fclose(lf);
    setenv("LITECRAB_MODEL", "env-model", 1);
    LiteCrabAppConfig c;
    char error[256];
    CHECK(!LiteCrabConfigLoad(&c, base, llm, error, sizeof error));
    CHECK(c.server.listenPort == 12345 && !strcmp(c.server.listenIp, "127.0.0.1"));
    CHECK(!strcmp(c.workspaceRoot, "/tmp/work") && !strcmp(c.llm.model, "env-model"));
    CHECK(c.llm.maxTokens == 321 && c.llm.stream == 0 && !strcmp(c.llm.apiKey, "test-only"));
    unsetenv("LITECRAB_MODEL");

    setenv("TEST_PROVIDER_KEY", "provider-secret", 1);
    FILE* strict = fopen(llm, "w");
    CHECK(strict != NULL);
    if (strict) {
        fputs("{\"llm\":{\"default\":\"first\",\"first\":{\"baseUrl\":\"http://localhost:1/v1/"
              "chat/completions\",\"apiKeyEnv\":\"TEST_PROVIDER_KEY\",\"modelName\":\"one\"},"
              "\"second\":{\"baseUrl\":\"https://example.test/v1/chat/completions\","
              "\"apiKey\":\"two-key\",\"modelName\":\"two\"},\"max_tokens\":456,"
              "\"temperature\":0.5,\"stream\":true,\"timeout_ms\":1234}}",
              strict);
        fclose(strict);
    }
    CHECK(!LiteCrabConfigLoadProvider(&c, NULL, llm, "first", error, sizeof error));
    CHECK(!strcmp(c.llm.model, "one") && !strcmp(c.llm.apiKey, "provider-secret"));
    CHECK(c.llm.maxTokens == 456 && c.llm.stream == 1 && c.llm.timeoutMs == 1234);
    CHECK(!LiteCrabConfigLoadProvider(&c, NULL, llm, "second", error, sizeof error));
    CHECK(!strcmp(c.llm.model, "two") && !strcmp(c.llm.apiKey, "two-key") &&
          !strcmp(c.llm.baseUrl, "https://example.test/v1/chat/completions"));
    CHECK(LiteCrabConfigLoadProvider(&c, NULL, llm, "missing", error, sizeof error) != 0);
    CHECK(strstr(error, "unknown default LLM provider") != NULL);
    unsetenv("TEST_PROVIDER_KEY");
    CHECK(LiteCrabConfigLoadProvider(&c, NULL, llm, "first", error, sizeof error) != 0);
    CHECK(strstr(error, "TEST_PROVIDER_KEY") != NULL);

    strict = fopen(llm, "w");
    CHECK(strict != NULL);
    if (strict) {
        fputs("{\"llm\":{\"max_tokens\":\"bad\"}}", strict);
        fclose(strict);
    }
    CHECK(LiteCrabConfigLoad(&c, NULL, llm, error, sizeof error) != 0);
    CHECK(strstr(error, "invalid LLM setting") != NULL);
    strict = fopen(llm, "w");
    CHECK(strict != NULL);
    if (strict) {
        fputs("{\"llm\":{\"max_tokens\":0,\"temperature\":3,\"timeout_ms\":0}}", strict);
        fclose(strict);
    }
    CHECK(LiteCrabConfigLoad(&c, NULL, llm, error, sizeof error) != 0);
    CHECK(strstr(error, "invalid LLM limits") != NULL);
    unlink(base);
    unlink(llm);
    WorkingMemoryInit();
    WorkingMemoryBeginRequest("choose");
    CHECK(WorkingMemoryParseOptions("1. alpha\n2) beta\n[3] gamma\ninvalid") == 3);
    const WorkingMemory* w = WorkingMemoryGet();
    CHECK(w->workflow.status == WORKFLOW_WAITING_INPUT &&
          !strcmp(w->workflow.options[2].label, "gamma"));
    for (int i = 0; i < 10; i++) {
        char name[32];
        snprintf(name, sizeof name, "tool%d", i);
        WorkingMemoryAfterTool(name, "{}", "ok", i);
    }
    w = WorkingMemoryGet();
    CHECK(w->workflow.toolHistoryCount == 8 && !strcmp(w->workflow.toolHistory[0].name, "tool2"));
    char longTask[300];
    memset(longTask, 'a', 253);
    strcpy(longTask + 253, "告警");
    WorkingMemoryBeginRequest(longTask);
    w = WorkingMemoryGet();
    CHECK(strlen(w->task) == 253 && w->task[252] == 'a');
    WorkingMemoryCacheSkill("demo", "instructions");
    char prompt[12000] = "base\n";
    CHECK(!WorkingMemoryAppendPrompt(prompt, sizeof prompt));
    CHECK(strstr(prompt, "Loaded skill: demo") && strstr(prompt, "instructions"));
    WorkingMemoryClose();
}

static void write_base_config(const char* path, const char* body) {
    FILE* f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(body, f);
        fclose(f);
    }
}

static void test_gateway_bind_config(void) {
    LiteCrabAppConfig c;
    LiteCrabConfigDefaults(&c);
    CHECK(!strcmp(c.server.listenIp, "127.0.0.1"));
    CHECK(c.server.allowUnauthenticatedRemote == 0);
    CHECK(c.server.recvTimeoutMs == 30000 && c.server.sendTimeoutMs == 30000);
    CHECK(c.server.maxReqBytes == 16384 && c.server.workerThreads == 4 &&
          c.server.maxConnections == 16);

    char base[] = "/tmp/litecrab-gateway-XXXXXX";
    int bfd = mkstemp(base);
    CHECK(bfd >= 0);
    close(bfd);

    write_base_config(base,
                      "{\"server\":{\"listen_ip\":\"0.0.0.0\"}}");
    char error[256];
    CHECK(LiteCrabConfigLoad(&c, base, NULL, error, sizeof error) != 0);
    CHECK(strstr(error, "unauthenticated remote TCP") != NULL);

    write_base_config(base,
                      "{\"server\":{\"listen_ip\":\"0.0.0.0\","
                      "\"allow_unauthenticated_remote\":true}}");
    CHECK(!LiteCrabConfigLoad(&c, base, NULL, error, sizeof error));
    CHECK(!strcmp(c.server.listenIp, "0.0.0.0"));
    CHECK(c.server.allowUnauthenticatedRemote == 1);

    write_base_config(base,
                      "{\"server\":{\"listen_ip\":\"0.0.0.0\","
                      "\"allow_unauthenticated_remote\":\"true\"}}");
    CHECK(LiteCrabConfigLoad(&c, base, NULL, error, sizeof error) != 0);
    CHECK(strstr(error, "allow_unauthenticated_remote") != NULL);

    write_base_config(base,
                      "{\"server\":{\"listen_ip\":\"0.0.0.0\","
                      "\"allow_unauthenticated_remote\":1}}");
    CHECK(LiteCrabConfigLoad(&c, base, NULL, error, sizeof error) != 0);
    CHECK(strstr(error, "allow_unauthenticated_remote") != NULL);

    write_base_config(base,
                      "{\"server\":{\"listen_ip\":\"not-an-ip\"}}");
    CHECK(LiteCrabConfigLoad(&c, base, NULL, error, sizeof error) != 0);
    CHECK(strstr(error, "invalid IPv4 listen address") != NULL);

    write_base_config(base,
                      "{\"server\":{\"listen_ip\":\"localhost\"}}");
    CHECK(LiteCrabConfigLoad(&c, base, NULL, error, sizeof error) != 0);
    CHECK(strstr(error, "invalid IPv4 listen address") != NULL);

    write_base_config(base,
                      "{\"server\":{\"listen_ip\":\"192.168.1.20\","
                      "\"allow_unauthenticated_remote\":false}}");
    CHECK(LiteCrabConfigLoad(&c, base, NULL, error, sizeof error) != 0);
    CHECK(strstr(error, "unauthenticated remote TCP") != NULL);

    write_base_config(base,
                      "{\"server\":{\"listen_ip\":\"127.0.0.2\"}}");
    CHECK(!LiteCrabConfigLoad(&c, base, NULL, error, sizeof error));
    CHECK(!strcmp(c.server.listenIp, "127.0.0.2"));
    CHECK(c.server.allowUnauthenticatedRemote == 0);

    write_base_config(base,
                      "{\"server\":{\"recv_timeout_ms\":99}}" );
    CHECK(LiteCrabConfigLoad(&c, base, NULL, error, sizeof error) != 0);
    write_base_config(base,
                      "{\"server\":{\"send_timeout_ms\":600001}}" );
    CHECK(LiteCrabConfigLoad(&c, base, NULL, error, sizeof error) != 0);
    write_base_config(base,
                      "{\"server\":{\"max_req_bytes\":16385}}" );
    CHECK(LiteCrabConfigLoad(&c, base, NULL, error, sizeof error) != 0);
    write_base_config(base,
                      "{\"server\":{\"worker_threads\":9}}" );
    CHECK(LiteCrabConfigLoad(&c, base, NULL, error, sizeof error) != 0);
    write_base_config(base,
                      "{\"server\":{\"worker_threads\":5,\"max_connections\":4}}" );
    CHECK(LiteCrabConfigLoad(&c, base, NULL, error, sizeof error) != 0);
    write_base_config(base,
                      "{\"server\":{\"worker_threads\":8,\"max_connections\":64,"
                      "\"recv_timeout_ms\":100,\"send_timeout_ms\":600000,"
                      "\"max_req_bytes\":16384}}" );
    CHECK(!LiteCrabConfigLoad(&c, base, NULL, error, sizeof error));
    CHECK(c.server.workerThreads == 8 && c.server.maxConnections == 64);

    unlink(base);
}

static void test_observability_rotation(void) {
    char directory[] = "/tmp/litecrab-log-rotation-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    CHECK(!InitLogWithDir(directory));
    char line[4090];
    memset(line, 'L', sizeof line - 1);
    line[sizeof line - 1] = 0;
    for (int i = 0; i < 2200; i++)
        LogPrint("[rotation] %s", line);
    CloseLog();
    DIR* opened = opendir(directory);
    CHECK(opened != NULL);
    int logFiles = 0, traceFiles = 0;
    unsigned long long logBytes = 0, traceBytes = 0;
    if (opened) {
        struct dirent* entry;
        while ((entry = readdir(opened))) {
            if (entry->d_name[0] == '.')
                continue;
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", directory, entry->d_name);
            struct stat status;
            if (stat(path, &status))
                continue;
            if (!strncmp(entry->d_name, "litecrab.log", 12)) {
                logFiles++;
                logBytes += (unsigned long long) status.st_size;
            } else if (!strncmp(entry->d_name, "agent_trace_", 12)) {
                traceFiles++;
                traceBytes += (unsigned long long) status.st_size;
            }
            unlink(path);
        }
        closedir(opened);
    }
    CHECK(logFiles <= 4 && logBytes <= 9ULL * 1024 * 1024);
    CHECK(traceFiles <= 4 && traceBytes <= 9ULL * 1024 * 1024);
    rmdir(directory);
}

static void test_streamed_tool_name(void) {
    LlmResponse r = {0};
    const char* sse = "data: "
                      "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c\","
                      "\"function\":{\"name\":\"read\",\"arguments\":\"{\"}}]}}]}\n\ndata: "
                      "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"\","
                      "\"function\":{\"name\":\"\",\"arguments\":\"}\"}}]}}]}\n\ndata: [DONE]\n\n";
    CHECK(!LlmParseResponseBody(sse, 1, &r));
    CHECK(r.callCount == 1 && !strcmp(r.calls[0].name, "read") && !strcmp(r.calls[0].id, "c") &&
          !strcmp(r.calls[0].input, "{}"));
    LlmResponseClear(&r);
}

static void test_shell_and_skill_tools(void) {
    char temp[] = "/tmp/litecrab-extra-XXXXXX";
    char* root = mkdtemp(temp);
    CHECK(root != NULL);
    char skills[512], alpha[512], beta[512], refs[512], manifest[640], ref[640], bad[512];
    CHECK(!test_path_join(skills, sizeof skills, root, "skills"));
    CHECK(!test_path_join(alpha, sizeof alpha, skills, "alpha"));
    CHECK(!test_path_join(beta, sizeof beta, skills, "beta"));
    CHECK(!test_path_join(refs, sizeof refs, alpha, "references"));
    CHECK(!test_path_join(bad, sizeof bad, skills, "bad-link"));
    CHECK(!mkdir(skills, 0700) && !mkdir(beta, 0700) && !mkdir(alpha, 0700) && !mkdir(refs, 0700));
    snprintf(manifest, sizeof manifest, "%s/SKILL.md", beta);
    FILE* f = fopen(manifest, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("---\nname: beta\ndescription: beta skill\n---\nBETA_BODY\n", f);
        fclose(f);
    }
    snprintf(manifest, sizeof manifest, "%s/SKILL.md", alpha);
    f = fopen(manifest, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("---\nname: alpha\ndescription: alpha skill\n---\nALPHA_BODY\n", f);
        fclose(f);
    }
    snprintf(ref, sizeof ref, "%s/info.txt", refs);
    f = fopen(ref, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("REFERENCE_OK", f);
        fclose(f);
    }
    CHECK(!symlink(alpha, bad));
    CHECK(SkillLoadAllFrom(skills) == 2);
    SkillEntry loaded[4];
    CHECK(SkillGetEntries(loaded, 4) == 2 && !strcmp(loaded[0].name, "alpha") &&
          !strcmp(loaded[1].name, "beta"));
    CHECK(strlen(loaded[0].definitionHash) == 64 && loaded[0].packageRoot[0] == '/');
    char skillPrompt[8192];
    CHECK(!ContextBuildSystemPromptEx(skillPrompt, sizeof skillPrompt, 1));
    const char* capability = strstr(skillPrompt, "You are LiteCrab");
    const char* tools = strstr(skillPrompt, "Available tools:");
    const char* runtimeRules = strstr(skillPrompt, "Runtime rules:");
    const char* availableSkills = strstr(skillPrompt, "Available runtime skills");
    const char* skillRules = strstr(skillPrompt, "Skill rules:");
    CHECK(capability && tools && runtimeRules && availableSkills && skillRules);
    CHECK(capability < tools && tools < runtimeRules && runtimeRules < availableSkills &&
          availableSkills < skillRules);
    CHECK(strstr(skillPrompt, "`grep({\"pattern\":\"TODO\"") &&
          strstr(skillPrompt, "No matches is a successful empty result"));
    CHECK(strstr(skillPrompt, "- `ls`: List the immediate contents") &&
          strstr(skillPrompt, "`ls({\"path\":\"src\"") &&
          strstr(skillPrompt, "- `pwd`: Return the agent workspace root") &&
          strstr(skillPrompt, "`pwd({})`"));
    CHECK(strstr(skillPrompt, "- `exec_program`: Execute a workspace binary directly") &&
          strstr(skillPrompt, "\"args\":[\"-o\",\"get_active_alarm\"]"));
    CHECK(!strstr(skillPrompt, "- `shell`") && !strstr(skillPrompt, "invoke `shell`"));
    CHECK(strstr(skillPrompt, "name: alpha") && strstr(skillPrompt, "alpha skill"));
    CHECK(strstr(skillPrompt, "function: alpha skill") &&
          strstr(skillPrompt, "`skill_read({\"skillPath\":\"alpha\""));
    CHECK(strstr(skillPrompt, "Load every skill instruction or reference file with `skill_read`") &&
          strstr(skillPrompt, "`references/通信异常.md`"));
    CHECK(strstr(skillPrompt,
                 "Read each file under `references/` with `skill_read` at most once per workflow") &&
          strstr(skillPrompt,
                 "never issue duplicate or otherwise unnecessary reads for the same reference file"));
    CHECK(!strstr(skillPrompt, "properties:") && !strstr(skillPrompt, "resumable="));
    char resolved[PATH_MAX];
    CHECK(realpath(skills, resolved) != NULL);
    CHECK(!strcmp(SkillGetCanonicalRoot(), resolved));
    char oldcwd[1024];
    CHECK(getcwd(oldcwd, sizeof oldcwd) != NULL);
    CHECK(!chdir("/tmp"));
    char* content = NULL;
    size_t contentSize = 0;
    int truncated = 0;
    CHECK(!SkillReadContent("alpha", "SKILL.md", &content, &contentSize, &truncated));
    CHECK(content && strstr(content, "ALPHA_BODY") && !truncated);
    free(content);
    content = NULL;
    CHECK(!SkillReadContent("alpha", "references/info.txt", &content, &contentSize, &truncated));
    CHECK(content && !strcmp(content, "REFERENCE_OK"));
    free(content);
    const char* rejected[] = {"",
                              ".",
                              "..",
                              "../beta/SKILL.md",
                              "/etc/passwd",
                              "C:/boot.ini",
                              "references/../SKILL.md",
                              "references//info.txt",
                              "references/./info.txt",
                              "scripts/run.sh",
                              "assets/a",
                              "SKILL.md/extra",
                              "references\\info.txt"};
    for (size_t i = 0; i < sizeof rejected / sizeof rejected[0]; i++)
        CHECK(SkillReadContent("alpha", rejected[i], &content, NULL, NULL) ==
              CRAB_ERROR_INVALID_ARG);
    char evil[640];
    snprintf(evil, sizeof evil, "%s/evil", refs);
    CHECK(!symlink(manifest, evil));
    CHECK(SkillReadContent("alpha", "references/evil", &content, NULL, NULL) ==
          CRAB_ERROR_EXECUTE_FAILED);
    unlink(evil);
    CHECK(!chdir(oldcwd));

    AgentSessionStateInit();
    CHECK(!AgentSessionStateOpen());
    AgentSessionStateSetSessionId("skill-session-a");
    CHECK(!SkillSupersetActivate("alpha", loaded[0].scriptsDir));
    char runId[64];
    snprintf(runId, sizeof runId, "%s", SkillSupersetGetActive()->basic.skillRunId);
    SkillRunSnapshot snap;
    CHECK(!SkillRunGetSnapshot("skill-session-a", runId, &snap) &&
          snap.status == SKILL_RUN_RUNNING);
    CHECK(!SkillSuspendQueuePush(SkillSupersetGetActive()));
    CHECK(!SkillSupersetFinish(SKILL_EXEC_PHASE_PARTIAL, "needs_input"));
    CHECK(!SkillRunGetSnapshot("skill-session-a", runId, &snap) &&
          snap.status == SKILL_RUN_WAITING_INPUT && snap.revision == 2);
    SkillRunSnapshot pending[4];
    CHECK(SkillRunListPending("skill-session-a", pending, 4) == 1);
    CHECK(pending[0].interruptionId[0] && pending[0].correlationToken[0]);
    SkillRouterResult route = SkillRouterRun("[]", "continue", pending[0].skillRunId, "", "", NULL);
    CHECK(route.action == SKILL_ROUTE_RESUME && !strcmp(route.skillName, "alpha") &&
          !strcmp(route.reasonCode, "run_id_match"));
    route = SkillRouterRun("[]", "continue", "", pending[0].interruptionId, "", NULL);
    CHECK(route.action == SKILL_ROUTE_RESUME && !strcmp(route.reasonCode, "interrupt_id_match"));
    route = SkillRouterRun("[]",
                           "continue",
                           pending[0].skillRunId,
                           pending[0].interruptionId,
                           pending[0].correlationToken,
                           NULL);
    CHECK(route.action == SKILL_ROUTE_RESUME && !strcmp(route.reasonCode, "interrupt_id_match"));
    route = SkillRouterRun("[]", "continue", "", "", pending[0].correlationToken, NULL);
    CHECK(route.action == SKILL_ROUTE_RESUME && !strcmp(route.reasonCode, "correlation_match"));
    const char* baseQueries[] = {"当前有哪些skill",
                                 "当前可以执行哪些指令",
                                 "你能做什么",
                                 "有哪些能力",
                                 "available commands",
                                 "what can you do"};
    for (size_t i = 0; i < sizeof baseQueries / sizeof baseQueries[0]; i++) {
        route = SkillRouterRun("[]", baseQueries[i], "", "", "", NULL);
        CHECK(route.action == SKILL_ROUTE_BASE && !strcmp(route.reasonCode, "base_intent_query") &&
              !strcmp(route.source, "rule"));
    }
    route = SkillRouterRun("[]", "start a completely unrelated task", "", "", "", NULL);
    CHECK(route.action != SKILL_ROUTE_RESUME);
    route = SkillRouterRun("[]", "继续", "", "", "", NULL);
    CHECK(route.action == SKILL_ROUTE_RESUME && !strcmp(route.skillName, "alpha"));
    CHECK(!SkillRouterRecover("alpha", route.skillRunId, NULL, "skill-session-a", "request-1"));
    CHECK(!SkillRunGetSnapshot("skill-session-a", runId, &snap) &&
          snap.status == SKILL_RUN_RUNNING);
    CHECK(!SkillSupersetFinish(SKILL_EXEC_PHASE_SUCCESS, "ok"));
    CHECK(!SkillRunGetSnapshot("skill-session-a", runId, &snap) &&
          snap.status == SKILL_RUN_COMPLETED);

    char sameRunOld[64], sameRunNew[64];
    CHECK(!SkillSupersetActivate("alpha", loaded[0].scriptsDir));
    snprintf(sameRunOld, sizeof sameRunOld, "%s", SkillSupersetGetActive()->basic.skillRunId);
    CHECK(!SkillSuspendQueuePush(SkillSupersetGetActive()));
    CHECK(!SkillSupersetFinish(SKILL_EXEC_PHASE_PARTIAL, "old-wait"));
    CHECK(!SkillSupersetActivate("alpha", loaded[0].scriptsDir));
    snprintf(sameRunNew, sizeof sameRunNew, "%s", SkillSupersetGetActive()->basic.skillRunId);
    CHECK(!SkillSuspendQueuePush(SkillSupersetGetActive()));
    CHECK(!SkillSupersetFinish(SKILL_EXEC_PHASE_PARTIAL, "new-wait"));
    SkillRunSnapshot oldPending, newPending;
    CHECK(!SkillRunGetSnapshot("skill-session-a", sameRunOld, &oldPending));
    CHECK(!SkillRunGetSnapshot("skill-session-a", sameRunNew, &newPending));
    CHECK(strcmp(oldPending.interruptionId, newPending.interruptionId));
    route = SkillRouterRun("[]", "continue", sameRunOld, newPending.interruptionId, "", NULL);
    CHECK(route.action == SKILL_ROUTE_ERROR &&
          !strcmp(route.reasonCode, "conflicting_route_references"));
    route = SkillRouterRun("[]", "continue", "", "", "", NULL);
    CHECK(route.action == SKILL_ROUTE_ERROR &&
          !strcmp(route.reasonCode, "ambiguous_waiting_runs"));
    route = SkillRouterRun("[]", "alpha", "", "", "", NULL);
    CHECK(route.action == SKILL_ROUTE_ERROR &&
          !strcmp(route.reasonCode, "ambiguous_waiting_skill"));
    route = SkillRouterRun("[]", "continue", sameRunOld, "", "", NULL);
    CHECK(route.action == SKILL_ROUTE_RESUME && !strcmp(route.skillRunId, sameRunOld));
    CHECK(!SkillRouterRecover("alpha", route.skillRunId, NULL, "skill-session-a", "request-exact"));
    CHECK(SkillSupersetGetActive() &&
          !strcmp(SkillSupersetGetActive()->basic.skillRunId, sameRunOld));
    CHECK(!SkillSupersetFinish(SKILL_EXEC_PHASE_SUCCESS, "exact-old"));
    CHECK(!SkillRunGetSnapshot("skill-session-a", sameRunOld, &snap) &&
          snap.status == SKILL_RUN_COMPLETED);
    CHECK(!SkillRunGetSnapshot("skill-session-a", sameRunNew, &snap) &&
          snap.status == SKILL_RUN_WAITING_INPUT);
    AgentSessionStateClose();
    SkillSuspendQueueClear();

    f = fopen(manifest, "a");
    CHECK(f != NULL);
    if (f) {
        fputs("MUTATED", f);
        fclose(f);
    }
    CHECK(SkillReadContent("alpha", "SKILL.md", &content, NULL, NULL) == CRAB_ERROR_PERMISSION);
    f = fopen(manifest, "w");
    CHECK(f != NULL);
    if (f) {
        fputs("---\nname: alpha\ndescription: alpha skill\n---\nALPHA_BODY\n", f);
        fclose(f);
    }
    CHECK(SkillLoadAllFrom(skills) == 2);

    CrabRuntime rt;
    CrabRuntimeConfig cfg = {.workspaceRoot = root};
    CHECK(!CrabRuntimeInit(&rt, &cfg));
    CHECK(!CrabRuntimeRegisterBuiltinTools(&rt));
    char shellMarker[512];
    CHECK(test_path_join(shellMarker, sizeof shellMarker, root, "shell-disabled-marker.txt") == 0);
    unlink(shellMarker);
    CrabToolCall c;
    CrabToolResponse r;
    call_init(&c, "shell");
    char script[768];
    snprintf(script, sizeof script, "printf shell-ok > %s", shellMarker);
    str_arg(&c, "script", script);
    CHECK(CrabRuntimeCallTool(&rt, &c, &r) == CRAB_ERROR_TOOL_DISABLED);
    CHECK(!r.success && strstr(r.summary, "tool is disabled"));
    CHECK(access(shellMarker, F_OK) != 0);
    call_init(&c, "skill_read");
    str_arg(&c, "skillPath", "alpha");
    CHECK(!CrabRuntimeCallTool(&rt, &c, &r));
    CHECK(r.success && strstr(r.content, "ALPHA_BODY"));
    call_init(&c, "read");
    str_arg(&c, "path", "skills/alpha/SKILL.md");
    CHECK(CrabRuntimeCallTool(&rt, &c, &r) == CRAB_ERROR_PERMISSION);
    CHECK(strstr(r.content, "skill instructions and references must use skill_read") &&
          strstr(r.content, "skillPath and fileName"));
    call_init(&c, "read");
    str_arg(&c, "path", "skills/alpha/references/info.txt");
    CHECK(CrabRuntimeCallTool(&rt, &c, &r) == CRAB_ERROR_PERMISSION);
    CHECK(strstr(r.content, "skill instructions and references must use skill_read") &&
          strstr(r.content, "skillPath and fileName"));
    call_init(&c, "skill_read");
    str_arg(&c, "skillPath", "alpha");
    str_arg(&c, "fileName", "../beta/SKILL.md");
    CHECK(CrabRuntimeCallTool(&rt, &c, &r) == CRAB_ERROR_INVALID_ARG);
    CrabRuntimeDestroy(&rt);
    unlink(bad);
    unlink(ref);
    rmdir(refs);
    snprintf(manifest, sizeof manifest, "%s/SKILL.md", alpha);
    unlink(manifest);
    snprintf(manifest, sizeof manifest, "%s/SKILL.md", beta);
    unlink(manifest);
    rmdir(alpha);
    rmdir(beta);
    rmdir(skills);
    rmdir(root);
}

int main(void) {
    test_json();
    test_runtime();
    test_hub();
    test_session_skill();
    test_session_isolation();
    test_session_persistence_and_scale();
    test_llm_parse();
    test_llm_trace_error_code();
    test_observability_rotation();
    test_config_working_memory();
    test_gateway_bind_config();
    test_streamed_tool_name();
    test_shell_and_skill_tools();
    fprintf(stderr, "LiteCrab tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
