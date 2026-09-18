/*
 * Built-in agent tools.
 *
 * Robot-control refactor: the Linux-only file and process tools (read,
 * csv_read, write, edit, grep, glob, ls, pwd, exec_program, shell) and their
 * fork/execve/waitpid/pipe/dup2/realpath machinery have been removed so the
 * remaining tool surface is portable to ESP32. What is left:
 *
 *   - skill_read:     load a routed skill's embedded instructions
 *   - skill_complete: mark the active skill run complete
 *
 * Robot control tools (self.otto.*) are NOT built in: they are device MCP
 * tools registered at runtime when a xiaozhi device connects (see
 * AgentLoopRegisterTool in the xiaozhi gateway path).
 */
#include "litecrab/kernel.h"
#include "litecrab/runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int raw_error(CrabRawResult* r, int code, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    r->exitCode = code;
    CrabRawResultSetStderr(r, b);
    return code;
}

static int do_skill_read(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    (void) r;
    const char *sp = CrabToolArgsGetString(c, "skillPath", NULL),
               *fn = CrabToolArgsGetString(c, "fileName", "SKILL.md");
    if (!sp || strstr(sp, "..") || sp[0] == '/' || strchr(sp, '\\') || strchr(sp, ':'))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "invalid skill name");
    if (!AgentSessionStateSkillAllowed(sp))
        return raw_error(raw, CRAB_ERROR_PERMISSION, "skill was not selected by router");
    char* d = NULL;
    size_t n;
    int tr, rc = SkillReadContent(sp, fn, &d, &n, &tr);
    if (rc)
        return raw_error(raw, rc, "skill content not found");
    /* Embedded skill bodies are stored without front matter; serve as-is. */
    raw->runtimeTruncated = tr;
    CrabRawResultSetStdout(raw, d);
    free(d);
    return 0;
}
static int do_skill_complete(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    (void) r;
    if (!SkillSupersetGetActive())
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "no active skill run");
    const char* summary = CrabToolArgsGetString(c, "summary", "completion requested");
    CrabRawResultSetStdout(raw, summary);
    return 0;
}

#define VSTR(s) {.type = CRAB_TOOL_VALUE_STRING, .stringValue = s}
static const CrabToolParamSpec sr_p[] = {
    {"skillPath",
     "skill name, for example Robot.Movement",
     CRAB_TOOL_VALUE_STRING,
     1,
     0,
     {0},
     0,
     0,
     NULL,
     0},
    {"fileName",
     "instruction resource; embedded skills expose SKILL.md",
     CRAB_TOOL_VALUE_STRING,
     0,
     1,
     VSTR("SKILL.md"),
     0,
     0,
     NULL,
     0}};
static const CrabToolParamSpec complete_p[] = {{"summary",
                                                "completion evidence summary",
                                                CRAB_TOOL_VALUE_STRING,
                                                0,
                                                1,
                                                VSTR("completed"),
                                                0,
                                                0,
                                                NULL,
                                                0}};
#define SPEC(n, d, en, ro, perm, conc, risk, p, fn) \
    {n, d, en, ro, perm, conc, risk, p, sizeof(p) / sizeof((p)[0]), fn, CrabBasicFilter}
static const CrabToolSpec tools[] = {
    SPEC("skill_read",
         "Load the routed skill's embedded instructions. Available only when the "
         "Skill Router selects or resumes a skill.",
         1,
         1,
         0,
         CRAB_TOOL_CONCURRENCY_SHARED_READ,
         CRAB_TOOL_RISK_LOW,
         sr_p,
         do_skill_read),
    SPEC("skill_complete",
         "Request completion of the active skill after its checks pass.",
         1,
         1,
         0,
         CRAB_TOOL_CONCURRENCY_SHARED_READ,
         CRAB_TOOL_RISK_LOW,
         complete_p,
         do_skill_complete)};
int CrabRuntimeRegisterBuiltinTools(CrabRuntime* r) {
    if (!r)
        return CRAB_ERROR_INVALID_ARG;
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; i++) {
        int rc = CrabRegistryRegister(&r->registry, &tools[i]);
        if (rc)
            return rc;
    }
    return 0;
}
