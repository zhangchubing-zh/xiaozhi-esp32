#include "litecrab/gateway.h"
#include "litecrab/json.h"
#include "litecrab/kernel.h"
#include "litecrab/runtime.h"
#include "litecrab/session.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int checks;
static int failures;
static const char* currentTest;

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        checks++;                                                                         \
        if (!(condition)) {                                                               \
            fprintf(stderr,                                                              \
                    "FAIL [%s] %s:%d: %s\n",                                             \
                    currentTest ? currentTest : "unknown",                               \
                    __FILE__,                                                             \
                    __LINE__,                                                             \
                    #condition);                                                          \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

static void begin_test(const char* name) {
    currentTest = name;
    printf("security-test: %s\n", name);
}

static void call_init(CrabToolCall* call, const char* toolName) {
    memset(call, 0, sizeof *call);
    snprintf(call->callId, sizeof call->callId, "security-test-call");
    snprintf(call->toolName, sizeof call->toolName, "%s", toolName);
}

static void string_arg(CrabToolCall* call, const char* name, const char* value) {
    CrabToolArg* arg = &call->args[call->argCount++];
    snprintf(arg->name, sizeof arg->name, "%s", name);
    arg->value.type = CRAB_TOOL_VALUE_STRING;
    arg->value.stringValue = value;
}

static int join_path(char* out, size_t outSize, const char* parent, const char* child) {
    int written = snprintf(out, outSize, "%s/%s", parent, child);
    return written < 0 || (size_t) written >= outSize ? -1 : 0;
}

static int path_missing(const char* path) {
    errno = 0;
    return access(path, F_OK) != 0 && errno == ENOENT;
}

static int init_runtime(CrabRuntime* runtime, const char* root, int readonlyMode) {
    CrabRuntimeConfig config = {.workspaceRoot = root, .readonlyMode = readonlyMode};
    int rc = CrabRuntimeInit(runtime, &config);
    if (!rc)
        rc = CrabRuntimeRegisterBuiltinTools(runtime);
    return rc;
}

static int stub_exec(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    (void) r;
    (void) c;
    raw->exitCode = 0;
    return 0;
}

static void test_path_escape_has_no_side_effect(const char* root) {
    begin_test("path escape is rejected without creating an outside file");
    char outsideName[128], outsidePath[512];
    snprintf(outsideName, sizeof outsideName, "litecrab-security-outside-%ld.txt", (long) getpid());
    snprintf(outsidePath, sizeof outsidePath, "/tmp/%s", outsideName);
    unlink(outsidePath);

    CrabRuntime runtime;
    CHECK(init_runtime(&runtime, root, 0) == 0);
    CrabToolCall call;
    CrabToolResponse response;
    call_init(&call, "write");
    char escapedPath[192];
    snprintf(escapedPath, sizeof escapedPath, "../%s", outsideName);
    string_arg(&call, "path", escapedPath);
    string_arg(&call, "content", "must-not-be-written");
    string_arg(&call, "mode", "create");

    int rc = CrabRuntimeCallTool(&runtime, &call, &response);
    CHECK(rc == CRAB_ERROR_INVALID_ARG);
    CHECK(response.toolExitCode == CRAB_ERROR_INVALID_ARG);
    CHECK(path_missing(outsidePath));
    CrabRuntimeDestroy(&runtime);
}

static void test_directory_symlink_escape_has_no_side_effect(const char* root) {
    begin_test("directory symlink escape is rejected without writing its target");
    char linkPath[512], outsideName[128], outsidePath[512];
    CHECK(join_path(linkPath, sizeof linkPath, root, "outside-link") == 0);
    snprintf(outsideName,
             sizeof outsideName,
             "litecrab-security-symlink-target-%ld.txt",
             (long) getpid());
    snprintf(outsidePath, sizeof outsidePath, "/tmp/%s", outsideName);
    unlink(linkPath);
    unlink(outsidePath);
    CHECK(symlink("/tmp", linkPath) == 0);

    CrabRuntime runtime;
    CHECK(init_runtime(&runtime, root, 0) == 0);
    CrabToolCall call;
    CrabToolResponse response;
    call_init(&call, "write");
    char toolPath[256];
    snprintf(toolPath, sizeof toolPath, "outside-link/%s", outsideName);
    string_arg(&call, "path", toolPath);
    string_arg(&call, "content", "must-not-follow-directory-symlink");
    string_arg(&call, "mode", "create");

    int rc = CrabRuntimeCallTool(&runtime, &call, &response);
    CHECK(rc == CRAB_ERROR_INVALID_ARG);
    CHECK(response.toolExitCode == CRAB_ERROR_INVALID_ARG);
    CHECK(path_missing(outsidePath));
    CrabRuntimeDestroy(&runtime);
    unlink(linkPath);
}

static void test_readonly_denial_has_no_side_effect(const char* root) {
    begin_test("readonly runtime rejects writes before execution");
    char target[512];
    CHECK(join_path(target, sizeof target, root, "readonly-denied.txt") == 0);
    unlink(target);

    CrabRuntime runtime;
    CHECK(init_runtime(&runtime, root, 1) == 0);
    CrabToolCall call;
    CrabToolResponse response;
    call_init(&call, "write");
    string_arg(&call, "path", "readonly-denied.txt");
    string_arg(&call, "content", "must-not-be-written");
    string_arg(&call, "mode", "create");

    int rc = CrabRuntimeCallTool(&runtime, &call, &response);
    CHECK(rc == CRAB_ERROR_PERMISSION);
    CHECK(response.runtimeError == CRAB_ERROR_PERMISSION);
    CHECK(response.toolExitCode == 0);
    CHECK(path_missing(target));
    CrabRuntimeDestroy(&runtime);
}

static void test_invalid_arguments_have_no_side_effect(const char* root) {
    begin_test("unknown tool arguments are rejected before execution");
    char target[512];
    CHECK(join_path(target, sizeof target, root, "invalid-args-denied.txt") == 0);
    unlink(target);

    CrabRuntime runtime;
    CHECK(init_runtime(&runtime, root, 0) == 0);
    CrabToolCall call;
    CrabToolResponse response;
    call_init(&call, "write");
    string_arg(&call, "path", "invalid-args-denied.txt");
    string_arg(&call, "content", "must-not-be-written");
    string_arg(&call, "unknownSecurityArgument", "not-allowed");

    int rc = CrabRuntimeCallTool(&runtime, &call, &response);
    CHECK(rc == CRAB_ERROR_INVALID_ARG);
    CHECK(response.runtimeError == CRAB_ERROR_INVALID_ARG);
    CHECK(response.toolExitCode == 0);
    CHECK(path_missing(target));
    CrabRuntimeDestroy(&runtime);
}

static void test_session_identifier_boundary(void) {
    begin_test("session identifiers reject path and control characters");
    CHECK(AgentSessionIdValidate("safe-session_1:device.2") == 0);
    CHECK(AgentSessionIdValidate("../escape") != 0);
    CHECK(AgentSessionIdValidate("nested/session") != 0);
    CHECK(AgentSessionIdValidate("line\nbreak") != 0);
    CHECK(AgentSessionIdValidate("") != 0);
}

static void test_disabled_tool_is_hidden_and_cannot_execute(const char* root) {
    begin_test("disabled tool is hidden and cannot execute");
    CrabRuntime runtime;
    CHECK(init_runtime(&runtime, root, 0) == 0);
    CrabToolCatalogView cat;
    CHECK(CrabRuntimeGetToolCatalog(&runtime, &cat) == 0);
    CHECK(cat.toolCount == 12);

    char schema[65536];
    CHECK(LlmToolAdapterRenderOpenaiToolsJson(&cat, schema, sizeof schema) == 0);
    CHECK(LjValidate(schema, LJ_ARRAY) == 0);
    CHECK(strstr(schema, "\"name\":\"shell\"") == NULL);
    CHECK(strstr(schema, "\"name\":\"exec_program\"") != NULL);
    CHECK(strstr(schema, "\"name\":\"read\"") != NULL);

    int rendered = 0;
    const char* p = schema;
    while ((p = strstr(p, "\"type\":\"function\""))) {
        rendered++;
        p += strlen("\"type\":\"function\"");
    }
    CHECK(rendered == 11);

    char marker[512];
    CHECK(join_path(marker, sizeof marker, root, "disabled-shell-marker.txt") == 0);
    unlink(marker);

    CrabToolCall call;
    CrabToolResponse response;
    call_init(&call, "shell");
    char script[768];
    snprintf(script, sizeof script, "printf disabled > '%s'", marker);
    string_arg(&call, "script", script);
    string_arg(&call, "path", ".");

    int rc = CrabRuntimeCallTool(&runtime, &call, &response);
    CHECK(rc == CRAB_ERROR_TOOL_DISABLED);
    CHECK(response.runtimeError == CRAB_ERROR_TOOL_DISABLED);
    CHECK(response.success == 0);
    CHECK(response.toolExitCode == 0);
    CHECK(strstr(response.summary, "tool is disabled") != NULL);
    CHECK(path_missing(marker));

    CrabRuntimeDestroy(&runtime);
}

static void test_disabled_tool_via_llm_adapter_parse(const char* root) {
    begin_test("forged shell call parsed via full catalog is rejected by runtime");
    CrabRuntime runtime;
    CHECK(init_runtime(&runtime, root, 0) == 0);
    CrabToolCatalogView cat;
    CHECK(CrabRuntimeGetToolCatalog(&runtime, &cat) == 0);

    char marker[512];
    CHECK(join_path(marker, sizeof marker, root, "forged-shell-marker.txt") == 0);
    unlink(marker);

    CrabToolCall call;
    char err[256];
    char args[768];
    snprintf(args, sizeof args, "{\"script\":\"printf forged > '%s'\",\"path\":\".\"}", marker);
    CHECK(LlmToolAdapterParseCall(&cat,
                                  "forged-shell-call",
                                  "shell",
                                  args,
                                  &call,
                                  err,
                                  sizeof err) == 0);

    CrabToolResponse response;
    int rc = CrabRuntimeCallTool(&runtime, &call, &response);
    CHECK(rc == CRAB_ERROR_TOOL_DISABLED);
    CHECK(response.runtimeError == CRAB_ERROR_TOOL_DISABLED);
    CHECK(response.toolExitCode == 0);
    CHECK(path_missing(marker));

    LlmToolAdapterFreeCall(&call);
    CrabRuntimeDestroy(&runtime);
}

static void test_invalid_enabled_by_default_is_rejected(const char* root) {
    begin_test("enabledByDefault must be strictly 0 or 1");
    CrabRuntime runtime;
    CHECK(init_runtime(&runtime, root, 0) == 0);
    size_t before = runtime.registry.toolCount;

    static const CrabToolParamSpec params[] = {
        {"path", "p", CRAB_TOOL_VALUE_STRING, 1, 0, {0}, 0, 0, NULL, 0}};
    CrabToolSpec bad = {"bad_enabled",
                        "invalid enabledByDefault",
                        2,
                        1,
                        0,
                        CRAB_TOOL_CONCURRENCY_SHARED_READ,
                        CRAB_TOOL_RISK_LOW,
                        params,
                        1,
                        stub_exec,
                        CrabBasicFilter};
    int rc = CrabRegistryRegister(&runtime.registry, &bad);
    CHECK(rc == CRAB_ERROR_INVALID_TOOL_SPEC);
    CHECK(runtime.registry.toolCount == before);

    CrabRuntimeDestroy(&runtime);
}

static void test_bind_policy_matrix(void) {
    begin_test("gateway bind policy matrix");
    struct Case {
        const char* ip;
        int allow;
        int expectAllow;
    } cases[] = {
        {"127.0.0.1", 0, 1},
        {"127.0.0.2", 0, 1},
        {"127.255.255.254", 0, 1},
        {"0.0.0.0", 0, 0},
        {"192.168.1.20", 0, 0},
        {"8.8.8.8", 0, 0},
        {"10.0.0.1", 0, 0},
        {"0.0.0.0", 1, 1},
        {"192.168.1.20", 1, 1},
        {"8.8.8.8", 1, 1},
        {"not-an-ip", 0, 0},
        {"not-an-ip", 1, 0},
        {"localhost", 0, 0},
        {"localhost", 1, 0},
        {"", 0, 0},
        {"127.0.0.1", 2, 0},
        {"127.0.0.1", -1, 0},
        {"127.0.0.1", 7, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        RequestServerConfig cfg = {0};
        snprintf(cfg.listenIp, sizeof cfg.listenIp, "%s", cases[i].ip);
        cfg.allowUnauthenticatedRemote = cases[i].allow;
        char err[256] = {0};
        int rc = RequestServerValidateBindPolicy(&cfg, err, sizeof err);
        if (cases[i].expectAllow) {
            CHECK(rc == 0);
        } else {
            CHECK(rc != 0);
            CHECK(err[0] != 0);
        }
    }

    char err[256];
    CHECK(RequestServerValidateBindPolicy(NULL, err, sizeof err) != 0);

    RequestServerConfig emptyIp = {0};
    emptyIp.allowUnauthenticatedRemote = 0;
    CHECK(RequestServerValidateBindPolicy(&emptyIp, err, sizeof err) != 0);
}

int main(void) {
    char rootTemplate[] = "/tmp/litecrab-security-tests-XXXXXX";
    char* root = mkdtemp(rootTemplate);
    if (!root) {
        perror("mkdtemp");
        return 1;
    }

    test_path_escape_has_no_side_effect(root);
    test_directory_symlink_escape_has_no_side_effect(root);
    test_readonly_denial_has_no_side_effect(root);
    test_invalid_arguments_have_no_side_effect(root);
    test_session_identifier_boundary();
    test_disabled_tool_is_hidden_and_cannot_execute(root);
    test_disabled_tool_via_llm_adapter_parse(root);
    test_invalid_enabled_by_default_is_rejected(root);
    test_bind_policy_matrix();

    if (rmdir(root) != 0) {
        fprintf(stderr, "FAIL [cleanup] cannot remove %s: %s\n", root, strerror(errno));
        failures++;
    }
    printf("security checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
