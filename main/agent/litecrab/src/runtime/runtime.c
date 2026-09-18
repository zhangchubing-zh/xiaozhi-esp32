#include "litecrab/runtime.h"

#include "litecrab/json.h"
#include "litecrab/working_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_text(char* d, size_t z, const char* s) {
    if (d && z)
        snprintf(d, z, "%s", s ? s : "");
}
int CrabRuntimeInit(CrabRuntime* r, const CrabRuntimeConfig* c) {
    if (!r)
        return CRAB_ERROR_INVALID_ARG;
    memset(r, 0, sizeof *r);
    CrabRegistryInit(&r->registry);
    if (c)
        r->config = *c;
    copy_text(
        r->workspaceRoot, sizeof r->workspaceRoot, c && c->workspaceRoot ? c->workspaceRoot : ".");
    copy_text(r->rawResultDir,
              sizeof r->rawResultDir,
              c && c->rawResultDir ? c->rawResultDir : ".crab/raw");
    r->config.workspaceRoot = r->workspaceRoot;
    r->config.rawResultDir = r->rawResultDir;
    if (r->config.maxResponseBytes <= 0 || r->config.maxResponseBytes > CRAB_TOOL_CONTENT_MAX)
        r->config.maxResponseBytes = CRAB_TOOL_CONTENT_MAX;
    if (r->config.maxRawBytes <= 0)
        r->config.maxRawBytes = 1024 * 1024;
    if (r->config.maxWarnings <= 0 || r->config.maxWarnings > CRAB_TOOL_WARNING_MAX)
        r->config.maxWarnings = CRAB_TOOL_WARNING_MAX;
    return 0;
}
int CrabRuntimeStart(CrabRuntime* r) {
    if (!r)
        return CRAB_ERROR_INVALID_ARG;
    r->started = 1;
    return 0;
}
void CrabRuntimeStop(CrabRuntime* r) {
    if (r)
        r->started = 0;
}
void CrabRuntimeDestroy(CrabRuntime* r) {
    if (r)
        memset(r, 0, sizeof *r);
}
const char* CrabRuntimeGetWorkspaceRoot(const CrabRuntime* r) {
    return r && r->workspaceRoot[0] ? r->workspaceRoot : ".";
}
void CrabRegistryInit(CrabToolRegistry* r) {
    if (r)
        memset(r, 0, sizeof *r);
}
const CrabToolSpec* CrabRegistryFind(const CrabToolRegistry* r, const char* n) {
    if (!r || !n)
        return NULL;
    for (size_t i = 0; i < r->toolCount; i++)
        if (!strcmp(r->tools[i].name, n))
            return &r->tools[i];
    return NULL;
}
int CrabRegistryRegister(CrabToolRegistry* r, const CrabToolSpec* t) {
    if (!r || !t)
        return CRAB_ERROR_INVALID_ARG;
    if (r->toolCount >= CRAB_REGISTRY_MAX_TOOLS)
        return CRAB_ERROR_REGISTRY_FULL;
    if (!t->name || !*t->name || !t->description || !*t->description || !t->execute || !t->filter)
        return CRAB_ERROR_INVALID_TOOL_SPEC;
    if (t->enabledByDefault != 0 && t->enabledByDefault != 1)
        return CRAB_ERROR_INVALID_TOOL_SPEC;
    if (CrabRegistryFind(r, t->name))
        return CRAB_ERROR_DUPLICATE_TOOL;
    r->tools[r->toolCount++] = *t;
    return 0;
}
int CrabToolSpecIsDefaultEnabled(const CrabToolSpec* spec) {
    return spec && spec->enabledByDefault == 1;
}
int CrabRegistryGetCatalog(const CrabToolRegistry* r, CrabToolCatalogView* c) {
    if (!r || !c)
        return CRAB_ERROR_INVALID_ARG;
    c->tools = r->tools;
    c->toolCount = r->toolCount;
    return 0;
}
int CrabRuntimeGetToolCatalog(const CrabRuntime* r, CrabToolCatalogView* c) {
    return r ? CrabRegistryGetCatalog(&r->registry, c) : CRAB_ERROR_INVALID_ARG;
}
const CrabToolArg* CrabToolCallFindArg(const CrabToolCall* c, const char* n) {
    if (!c || !n)
        return NULL;
    size_t count = c->argCount > CRAB_TOOL_MAX_ARGS ? CRAB_TOOL_MAX_ARGS : c->argCount;
    for (size_t i = 0; i < count; i++)
        if (!strcmp(c->args[i].name, n))
            return &c->args[i];
    return NULL;
}
const CrabToolParamSpec* CrabToolSpecFindParam(const CrabToolSpec* t, const char* n) {
    if (!t || !n)
        return NULL;
    for (size_t i = 0; i < t->paramCount; i++)
        if (t->params[i].name && !strcmp(t->params[i].name, n))
            return &t->params[i];
    return NULL;
}
static int argerr(char* e, size_t z, const char* m, const char* n) {
    if (e && z)
        snprintf(e, z, n ? "%s: %s" : "%s", m, n ? n : "");
    return CRAB_ERROR_INVALID_ARG;
}
int CrabToolValidateCall(const CrabToolSpec* t, const CrabToolCall* c, char* e, size_t z) {
    if (!t || !c)
        return argerr(e, z, "invalid call", NULL);
    if (!t->name || strcmp(c->toolName, t->name))
        return argerr(e, z, "tool name mismatch", NULL);
    if (c->argCount > CRAB_TOOL_MAX_ARGS)
        return argerr(e, z, "too many arguments", NULL);
    for (size_t i = 0; i < c->argCount; i++) {
        const CrabToolArg* a = &c->args[i];
        if (!a->name[0])
            return argerr(e, z, "empty argument name", NULL);
        for (size_t j = 0; j < i; j++)
            if (!strcmp(a->name, c->args[j].name))
                return argerr(e, z, "duplicate argument", a->name);
        const CrabToolParamSpec* p = CrabToolSpecFindParam(t, a->name);
        if (!p)
            return argerr(e, z, "unknown argument", a->name);
        if (a->value.type != p->type)
            return argerr(e, z, "invalid argument type", a->name);
        if (p->type == CRAB_TOOL_VALUE_STRING) {
            if (!a->value.stringValue)
                return argerr(e, z, "missing string value", a->name);
            if (p->enumCount) {
                int found = 0;
                for (size_t j = 0; j < p->enumCount; j++)
                    if (!strcmp(a->value.stringValue, p->enumValues[j]))
                        found = 1;
                if (!found)
                    return argerr(e, z, "invalid enum value", a->name);
            }
        }
        if (p->type == CRAB_TOOL_VALUE_INT && (p->minInt || p->maxInt) &&
            (a->value.intValue < p->minInt || a->value.intValue > p->maxInt))
            return argerr(e, z, "integer out of range", a->name);
        if (p->type == CRAB_TOOL_VALUE_STRING_ARRAY) {
            size_t count = a->value.stringArrayValue.count;
            size_t totalBytes = 0;
            if (count > CRAB_TOOL_ARRAY_MAX_ITEMS)
                return argerr(e, z, "array item count out of range", a->name);
            for (size_t j = 0; j < count; j++) {
                const char* item = a->value.stringArrayValue.items[j];
                size_t itemBytes = item ? strlen(item) : 0;
                if (!item || itemBytes > CRAB_TOOL_ARRAY_MAX_ITEM_BYTES)
                    return argerr(e, z, "invalid array item", a->name);
                totalBytes += itemBytes;
            }
            if (totalBytes > CRAB_TOOL_ARRAY_MAX_TOTAL_BYTES)
                return argerr(e, z, "array content is too large", a->name);
        }
    }
    for (size_t i = 0; i < t->paramCount; i++)
        if (t->params[i].required && !CrabToolCallFindArg(c, t->params[i].name))
            return argerr(e, z, "missing required argument", t->params[i].name);
    if (e && z)
        e[0] = 0;
    return 0;
}
const char* CrabToolArgsGetString(const CrabToolCall* c, const char* n, const char* d) {
    const CrabToolArg* a = CrabToolCallFindArg(c, n);
    return a && a->value.type == CRAB_TOOL_VALUE_STRING && a->value.stringValue
               ? a->value.stringValue
               : d;
}
int64_t CrabToolArgsGetInt(const CrabToolCall* c, const char* n, int64_t d) {
    const CrabToolArg* a = CrabToolCallFindArg(c, n);
    return a && a->value.type == CRAB_TOOL_VALUE_INT ? a->value.intValue : d;
}
int CrabToolArgsGetBool(const CrabToolCall* c, const char* n, int d) {
    const CrabToolArg* a = CrabToolCallFindArg(c, n);
    return a && a->value.type == CRAB_TOOL_VALUE_BOOL ? !!a->value.boolValue : d;
}
const CrabToolStringArray* CrabToolArgsGetStringArray(const CrabToolCall* c, const char* n) {
    const CrabToolArg* a = CrabToolCallFindArg(c, n);
    return a && a->value.type == CRAB_TOOL_VALUE_STRING_ARRAY ? &a->value.stringArrayValue : NULL;
}
void CrabRawResultClear(CrabRawResult* r) {
    if (r) {
        free(r->stdoutData);
        free(r->stderrData);
        memset(r, 0, sizeof *r);
    }
}
static int set_heap(char** p, size_t* z, const char* d) {
    char* x = strdup(d ? d : "");
    if (!x)
        return CRAB_ERROR_NO_MEMORY;
    free(*p);
    *p = x;
    *z = strlen(x);
    return 0;
}
int CrabRawResultSetStdout(CrabRawResult* r, const char* d) {
    return r ? set_heap(&r->stdoutData, &r->stdoutSize, d) : CRAB_ERROR_INVALID_ARG;
}
int CrabRawResultSetStderr(CrabRawResult* r, const char* d) {
    return r ? set_heap(&r->stderrData, &r->stderrSize, d) : CRAB_ERROR_INVALID_ARG;
}
int CrabFilteredResultSetSummary(CrabFilteredResult* f, const char* s) {
    if (!f)
        return CRAB_ERROR_INVALID_ARG;
    copy_text(f->summary, sizeof f->summary, s);
    return 0;
}
int CrabFilteredResultSetContent(CrabFilteredResult* f, const char* s) {
    if (!f)
        return CRAB_ERROR_INVALID_ARG;
    copy_text(f->content, sizeof f->content, s);
    f->contentSize = strlen(f->content);
    f->truncated = f->truncated || (s && strlen(s) >= sizeof f->content);
    return 0;
}
int CrabFilteredResultAddWarning(CrabFilteredResult* f, const char* w) {
    if (!f || f->warningCount >= CRAB_TOOL_WARNING_MAX)
        return CRAB_ERROR_INVALID_ARG;
    copy_text(f->warnings[f->warningCount++], CRAB_TOOL_WARNING_TEXT_MAX, w);
    return 0;
}
int CrabBasicFilter(const CrabToolCall* c, const CrabRawResult* r, CrabFilteredResult* f) {
    (void) c;
    if (!r || !f)
        return CRAB_ERROR_INVALID_ARG;
    memset(f, 0, sizeof *f);
    f->success = r->exitCode == 0;
    f->truncated = r->runtimeTruncated;
    f->rawAvailable = !!r->rawRef[0];
    CrabFilteredResultSetSummary(
        f,
        r->exitCode == 0 ? "tool completed"
                         : (r->stderrData && *r->stderrData ? r->stderrData : "tool failed"));
    CrabFilteredResultSetContent(f, r->stdoutData ? r->stdoutData : "");
    if (r->exitCode == 0 && r->stderrData && *r->stderrData)
        CrabFilteredResultAddWarning(f, r->stderrData);
    return 0;
}
static void response_init(CrabToolResponse* r, const CrabToolCall* c) {
    memset(r, 0, sizeof *r);
    if (c) {
        copy_text(r->callId, sizeof r->callId, c->callId);
        copy_text(r->toolName, sizeof r->toolName, c->toolName);
    }
}
int CrabBuildToolResponse(const CrabToolSpec* t,
                          const CrabToolCall* c,
                          const CrabRawResult* raw,
                          const CrabFilteredResult* f,
                          CrabToolResponse* r) {
    (void) t;
    if (!c || !raw || !f || !r)
        return CRAB_ERROR_INVALID_ARG;
    response_init(r, c);
    r->success = f->success;
    r->toolExitCode = raw->exitCode;
    r->truncated = f->truncated;
    r->warningCount =
        f->warningCount > CRAB_TOOL_WARNING_MAX ? CRAB_TOOL_WARNING_MAX : f->warningCount;
    copy_text(r->summary, sizeof r->summary, f->summary);
    copy_text(r->rawRef, sizeof r->rawRef, raw->rawRef);
    for (int i = 0; i < r->warningCount; i++)
        copy_text(r->warnings[i], sizeof r->warnings[i], f->warnings[i]);
    LjBuf b;
    LjBufInit(&b, r->content, sizeof r->content);
    LjAppend(&b,
             "tool: %s\nsuccess: %s\nexit_code: %d\n\nsummary:\n  %s\n\ncontent:\n%s\n",
             c->toolName,
             r->success ? "true" : "false",
             raw->exitCode,
             f->summary,
             f->content);
    if (r->warningCount) {
        LjAppend(&b, "\nwarnings:\n");
        for (int i = 0; i < r->warningCount; i++)
            LjAppend(&b, "  %s\n", r->warnings[i]);
    }
    LjAppend(&b, "\ntruncated: %s\nraw_ref: %s\n", r->truncated ? "true" : "false", raw->rawRef);
    if (b.failed) {
        r->truncated = 1;
        r->content[sizeof r->content - 1] = 0;
    }
    return 0;
}
int CrabBuildRuntimeError(CrabToolResponse* r, const CrabToolCall* c, int code, const char* m) {
    if (!r || !c)
        return CRAB_ERROR_INVALID_ARG;
    response_init(r, c);
    r->runtimeError = code;
    copy_text(r->summary, sizeof r->summary, m ? m : "runtime error");
    snprintf(r->content,
             sizeof r->content,
             "tool: %s\nsuccess: false\nruntime_error: %d\n\nsummary:\n  %s\n",
             c->toolName,
             code,
             r->summary);
    return code;
}
int CrabBuildFallbackResponse(const CrabToolSpec* t,
                              const CrabToolCall* c,
                              const CrabRawResult* raw,
                              CrabToolResponse* r) {
    CrabFilteredResult f;
    memset(&f, 0, sizeof f);
    f.truncated = 1;
    copy_text(f.summary, sizeof f.summary, "filter failed; returning fallback summary");
    copy_text(f.content, sizeof f.content, raw && raw->stderrData ? raw->stderrData : "");
    CrabBuildToolResponse(t, c, raw, &f, r);
    r->filterError = CRAB_ERROR_FILTER_FAILED;
    return CRAB_ERROR_FILTER_FAILED;
}
int CrabRuntimeCallTool(CrabRuntime* rt, const CrabToolCall* c, CrabToolResponse* r) {
    if (!rt || !c || !r)
        return CRAB_ERROR_INVALID_ARG;
    CrabRawResult raw = {0};
    CrabFilteredResult f = {0};
    const CrabToolSpec* t = CrabRegistryFind(&rt->registry, c->toolName);
    if (!t)
        return CrabBuildRuntimeError(r, c, CRAB_ERROR_TOOL_NOT_FOUND, "tool not found");
    if (!CrabToolSpecIsDefaultEnabled(t))
        return CrabBuildRuntimeError(r, c, CRAB_ERROR_TOOL_DISABLED, "tool is disabled");
    if (rt->config.readonlyMode && !t->readonly)
        return CrabBuildRuntimeError(r, c, CRAB_ERROR_PERMISSION, "runtime is read-only");
    char err[256];
    int rc = CrabToolValidateCall(t, c, err, sizeof err);
    if (rc)
        return CrabBuildRuntimeError(r, c, rc, err);
    WorkingMemoryBeforeTool(c->toolName, NULL);
    int ex = t->execute(rt, c, &raw);
    if (ex && raw.exitCode == 0)
        raw.exitCode = ex;
    int fr = t->filter(c, &raw, &f);
    if (fr)
        CrabBuildFallbackResponse(t, c, &raw, r);
    else
        CrabBuildToolResponse(t, c, &raw, &f, r);
    if (!strcmp(c->toolName, "skill_read") && !ex)
        for (size_t i = 0; i < c->argCount; i++)
            if (!strcmp(c->args[i].name, "skillPath") &&
                c->args[i].value.type == CRAB_TOOL_VALUE_STRING)
                WorkingMemoryCacheSkill(c->args[i].value.stringValue, r->content);
    WorkingMemoryAfterTool(c->toolName, NULL, r->content, r->toolExitCode);
    CrabRawResultClear(&raw);
    return ex;
}
