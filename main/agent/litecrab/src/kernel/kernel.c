#include "litecrab/kernel.h"

#include "litecrab/hub.h"
#include "litecrab/json.h"
#include "litecrab/observability.h"

#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static int parse_tokens(const char* j, LjToken** out) {
    if (!j)
        return -1;
    const char* p = j;
    while (*p && strchr(" \r\n\t", *p))
        p++;
    LjType root = *p == '[' ? LJ_ARRAY : LJ_OBJECT;
    if (LjValidate(j, root))
        return -1;
    size_t cap = strlen(j) / 2 + 64;
    LjToken* t = calloc(cap, sizeof *t);
    if (!t)
        return -1;
    LjParser parser;
    LjInit(&parser);
    int n = LjParse(&parser, j, strlen(j), t, (unsigned) cap);
    if (n < 0) {
        free(t);
        return -1;
    }
    *out = t;
    return n;
}
int JsonExtractStringField(const char* j, const char* field, char* out, size_t z) {
    if (!j || !field || !out || !z)
        return 0;
    LjToken* t = NULL;
    int n = parse_tokens(j, &t);
    if (n < 1 || t[0].type != LJ_OBJECT) {
        free(t);
        return 0;
    }
    int v = LjObjectGet(j, t, n, 0, field);
    int ok = v >= 0 && t[v].type == LJ_STRING && !LjString(j, &t[v], out, z);
    free(t);
    return ok;
}
int JsonExtractSkillPath(const char* j, char* out, size_t z) {
    return JsonExtractStringField(j, "skillPath", out, z);
}
int TextExtractMarkerInt(const char* t, const char* m, int* out) {
    if (!t || !m || !out)
        return 0;
    const char* p = strstr(t, m);
    if (!p)
        return 0;
    p += strlen(m);
    while (*p == ' ' || *p == '\t')
        p++;
    char* e;
    long v = strtol(p, &e, 10);
    if (e == p)
        return 0;
    *out = (int) v;
    return 1;
}

static const char* type_name(CrabToolValueType t) {
    return t == CRAB_TOOL_VALUE_INT          ? "integer"
           : t == CRAB_TOOL_VALUE_BOOL       ? "boolean"
           : t == CRAB_TOOL_VALUE_STRING_ARRAY ? "array"
                                                : "string";
}
int LlmToolAdapterRenderOpenaiToolsJsonFiltered(const CrabToolCatalogView* c,
                                                int includeSkillTools,
                                                char* out,
                                                size_t z) {
    if (!c || !out || !z)
        return -1;
    LjBuf b;
    LjBufInit(&b, out, z);
    LjAppend(&b, "[");
    size_t emitted = 0;
    for (size_t i = 0; i < c->toolCount; i++) {
        const CrabToolSpec* t = &c->tools[i];
        if (!CrabToolSpecIsDefaultEnabled(t))
            continue;
        if (!includeSkillTools &&
            (!strcmp(t->name, "skill_read") || !strcmp(t->name, "skill_complete")))
            continue;
        if (emitted++)
            LjAppend(&b, ",");
        LjAppend(&b, "{\"type\":\"function\",\"function\":{\"name\":");
        LjAppendJsonString(&b, t->name);
        LjAppend(&b, ",\"description\":");
        LjAppendJsonString(&b, t->description);
        LjAppend(&b, ",\"parameters\":{\"type\":\"object\",\"properties\":{");
        for (size_t j = 0; j < t->paramCount; j++) {
            const CrabToolParamSpec* p = &t->params[j];
            if (j)
                LjAppend(&b, ",");
            LjAppendJsonString(&b, p->name);
            LjAppend(&b, ":{\"type\":\"%s\",\"description\":", type_name(p->type));
            LjAppendJsonString(&b, p->description);
            if (p->type == CRAB_TOOL_VALUE_STRING_ARRAY) {
                LjAppend(&b, ",\"items\":{\"type\":\"string\"}");
                LjAppend(&b, ",\"maxItems\":%d", CRAB_TOOL_ARRAY_MAX_ITEMS);
            }
            if (p->type == CRAB_TOOL_VALUE_STRING && p->enumCount) {
                LjAppend(&b, ",\"enum\":[");
                for (size_t k = 0; k < p->enumCount; k++) {
                    if (k)
                        LjAppend(&b, ",");
                    LjAppendJsonString(&b, p->enumValues[k]);
                }
                LjAppend(&b, "]");
            }
            if (p->type == CRAB_TOOL_VALUE_INT && (p->minInt || p->maxInt))
                LjAppend(&b,
                         ",\"minimum\":%lld,\"maximum\":%lld",
                         (long long) p->minInt,
                         (long long) p->maxInt);
            LjAppend(&b, "}");
        }
        LjAppend(&b, "},\"required\":[");
        int comma = 0;
        for (size_t j = 0; j < t->paramCount; j++)
            if (t->params[j].required) {
                if (comma++)
                    LjAppend(&b, ",");
                LjAppendJsonString(&b, t->params[j].name);
            }
        LjAppend(&b, "],\"additionalProperties\":false}}}");
    }
    LjAppend(&b, "]");
    return b.failed ? -3 : 0;
}
int LlmToolAdapterRenderOpenaiToolsJson(const CrabToolCatalogView* c, char* out, size_t z) {
    return LlmToolAdapterRenderOpenaiToolsJsonFiltered(c, 1, out, z);
}
void LlmToolAdapterFreeCall(CrabToolCall* c) {
    if (!c)
        return;
    for (size_t i = 0; i < c->argCount && i < CRAB_TOOL_MAX_ARGS; i++)
        if (c->args[i].value.type == CRAB_TOOL_VALUE_STRING)
            free((void*) c->args[i].value.stringValue);
        else if (c->args[i].value.type == CRAB_TOOL_VALUE_STRING_ARRAY)
            for (size_t j = 0; j < c->args[i].value.stringArrayValue.count; j++)
                free((void*) c->args[i].value.stringArrayValue.items[j]);
    memset(c, 0, sizeof *c);
}
int LlmToolAdapterParseCall(const CrabToolCatalogView* cat,
                            const char* id,
                            const char* name,
                            const char* args,
                            CrabToolCall* c,
                            char* err,
                            size_t ez) {
    if (!cat || !name || !c)
        return -1;
    memset(c, 0, sizeof *c);
    snprintf(c->callId, sizeof c->callId, "%s", id ? id : "");
    snprintf(c->toolName, sizeof c->toolName, "%s", name);
    const CrabToolSpec* tool = NULL;
    for (size_t i = 0; i < cat->toolCount; i++)
        if (!strcmp(cat->tools[i].name, name)) {
            tool = &cat->tools[i];
            break;
        }
    if (!tool) {
        snprintf(err, ez, "tool not found");
        return -1;
    }
    const char* j = args ? args : "{}";
    LjToken* t;
    int n = parse_tokens(j, &t);
    if (n < 1 || t[0].type != LJ_OBJECT) {
        snprintf(err, ez, "invalid tool arguments JSON");
        free(t);
        return -1;
    }
    int i = 1;
    while (i < n && t[i].start < t[0].end) {
        if (c->argCount >= CRAB_TOOL_MAX_ARGS) {
            snprintf(err, ez, "too many tool arguments");
            goto fail;
        }
        char key[CRAB_TOOL_ARG_NAME_MAX];
        if (t[i].type != LJ_STRING || LjString(j, &t[i], key, sizeof key)) {
            snprintf(err, ez, "invalid argument name");
            goto fail;
        }
        int v = i + 1;
        const CrabToolParamSpec* p = CrabToolSpecFindParam(tool, key);
        if (!p) {
            snprintf(err, ez, "unknown tool argument: %s", key);
            goto fail;
        }
        CrabToolArg* a = &c->args[c->argCount];
        snprintf(a->name, sizeof a->name, "%s", key);
        a->value.type = p->type;
        if (p->type == CRAB_TOOL_VALUE_STRING) {
            if (t[v].type != LJ_STRING) {
                snprintf(err, ez, "string argument expected");
                goto fail;
            }
            size_t z = (size_t) (t[v].end - t[v].start) + 1;
            char* s = malloc(z);
            if (!s || LjString(j, &t[v], s, z)) {
                free(s);
                snprintf(err, ez, "out of memory");
                goto fail;
            }
            a->value.stringValue = s;
        } else if (p->type == CRAB_TOOL_VALUE_INT) {
            if (LjInt64(j, &t[v], &a->value.intValue)) {
                snprintf(err, ez, "integer argument expected");
                goto fail;
            }
        } else if (p->type == CRAB_TOOL_VALUE_BOOL) {
            if (LjBool(j, &t[v], &a->value.boolValue)) {
                snprintf(err, ez, "boolean argument expected");
                goto fail;
            }
        } else if (p->type == CRAB_TOOL_VALUE_STRING_ARRAY) {
            if (t[v].type != LJ_ARRAY || t[v].size > CRAB_TOOL_ARRAY_MAX_ITEMS) {
                snprintf(err, ez, "string array argument expected");
                goto fail;
            }
            for (int arrayIndex = 0; arrayIndex < t[v].size; arrayIndex++) {
                int item = LjArrayGet(t, n, v, arrayIndex);
                if (item < 0 || t[item].type != LJ_STRING) {
                    snprintf(err, ez, "string array item expected");
                    for (size_t k = 0; k < a->value.stringArrayValue.count; k++)
                        free((void*) a->value.stringArrayValue.items[k]);
                    a->value.stringArrayValue.count = 0;
                    goto fail;
                }
                size_t itemSize = (size_t) (t[item].end - t[item].start) + 1;
                char* text = malloc(itemSize);
                if (!text || LjString(j, &t[item], text, itemSize)) {
                    free(text);
                    snprintf(err, ez, "out of memory");
                    for (size_t k = 0; k < a->value.stringArrayValue.count; k++)
                        free((void*) a->value.stringArrayValue.items[k]);
                    a->value.stringArrayValue.count = 0;
                    goto fail;
                }
                a->value.stringArrayValue.items[a->value.stringArrayValue.count++] = text;
            }
        } else {
            snprintf(err, ez, "unsupported argument type");
            goto fail;
        }
        c->argCount++;
        i = LjSkip(t, n, v);
    }
    free(t);
    return 0;
fail:
    free(t);
    LlmToolAdapterFreeCall(c);
    return -1;
}

static int array_append(char* json, size_t z, const char* obj) {
    if (!json || !obj || LjValidate(json, LJ_ARRAY) || LjValidate(obj, LJ_OBJECT))
        return -1;
    size_t n = strlen(json), o = strlen(obj);
    while (n && strchr(" \r\n\t", json[n - 1]))
        n--;
    if (!n || json[n - 1] != ']')
        return -1;
    size_t p = n - 1;
    int empty = 1;
    for (size_t i = 0; i < p; i++)
        if (json[i] == '[') {
        } else if (!strchr(" \r\n\t", json[i])) {
            empty = 0;
            break;
        }
    if (p + o + 3 > z)
        return -1;
    json[p] = 0;
    snprintf(json + p, z - p, "%s%s]", empty ? "" : ",", obj);
    return 0;
}
static int append_role(char* j, size_t z, const char* role, const char* content) {
    char* obj = malloc(strlen(content ? content : "") * 6 + 128);
    if (!obj)
        return -1;
    LjBuf b;
    LjBufInit(&b, obj, strlen(content ? content : "") * 6 + 128);
    LjAppend(&b, "{\"role\":");
    LjAppendJsonString(&b, role);
    LjAppend(&b, ",\"content\":");
    LjAppendJsonString(&b, content ? content : "");
    LjAppend(&b, "}");
    int rc = b.failed ? -1 : array_append(j, z, obj);
    free(obj);
    return rc;
}
int MessageTreeAppendAssistant(char* j, size_t z, const char* c) {
    return append_role(j, z, "assistant", c);
}
int MessageTreeAppendUser(char* j, size_t z, const char* c) {
    WorkingMemoryBeginRequest(c);
    return append_role(j, z, "user", c);
}

int ContextBuildSystemPromptEx(char* b, size_t z, int skills) {
    static const char base[] =
        "# LiteCrab\n\n"
        "You are LiteCrab, the agent running on the Otto bipedal robot. You control the "
        "robot through device tools (for example `self.otto.action`, "
        "`self.otto.servo_sequences`, `self.battery.get_level`) that are provided in the "
        "current API tool list, and you follow router-selected skill workflows for robot "
        "movement, poses, status, and calibration tasks.\n\n"
        "Built-in tools:\n"
        "- `skill_read`: Load the routed skill's embedded instructions. It is available "
        "only when the Skill Router selects or resumes a skill. Example: "
        "`skill_read({\"skillPath\":\"Robot.Movement\",\"fileName\":\"SKILL.md\"})`.\n"
        "- `skill_complete`: Mark the active skill complete after all of its checks pass. "
        "It is available only in a routed skill workflow. Example: "
        "`skill_complete({\"summary\":\"Movement executed and confirmed.\"})`.\n"
        "Robot control tools (`self.otto.*` and related) appear in the same API tool list "
        "when the device is connected.\n"
        "Only call tools included in the current API tool list; that list is authoritative "
        "for this turn.\n\n"
        "Runtime rules:\n"
        "- Use the device tools to execute robot actions; never claim an action is done "
        "before the tool call succeeds and you have inspected its result.\n"
        "- Treat tool outputs as intermediate state unless they fully answer the user "
        "request.\n"
        "- Final answers should be brief and should not repeat large tool outputs.\n";
    LjBuf out;
    LjBufInit(&out, b, z);
    LjAppend(&out, "%s", base);
    if (skills) {
        SkillEntry e[32];
        int n = SkillGetEntries(e, 32);
        if (n > 0) {
            LjAppend(&out,
                     "\nAvailable runtime skills (selection metadata only):\n"
                     "The Skill Router, not the assistant, decides which Skill may run. Each "
                     "description explains its function; the example shows how to load it after "
                     "the router selects it.\n");
            for (int i = 0; i < n; i++)
                LjAppend(&out,
                         "\n## skill_%d\nname: %s\nfunction: %s\nusage example: "
                         "`skill_read({\"skillPath\":\"%s\",\"fileName\":\"SKILL.md\"})`\n",
                         i,
                         e[i].name,
                         e[i].prompt,
                         e[i].name);
        } else {
            LjAppend(&out, "\nAvailable runtime skills:\n- None loaded.\n");
        }
    }
    LjAppend(&out,
             "\nSkill rules:\n- Always call `skill_read` before executing any skill workflow, "
             "even if you recall it from conversation history.\n- Use the skill list only for "
             "understanding capabilities; obey the Skill Router decision and never self-select "
             "or switch skills.\n- When a skill is selected, call `skill_read` with the selected "
             "skill name as `skillPath`.\n- After satisfying the loaded skill's completion "
             "checks, call `skill_complete` with a short evidence summary before the final "
             "answer.\n");
    if (out.failed)
        return -1;
    return WorkingMemoryAppendPrompt(b, z);
}

static CrabRuntime runtime;
static char toolsJson[8192] EXT_RAM_BSS_ATTR;
static char baseToolsJson[8192] EXT_RAM_BSS_ATTR;
/* Guards toolsJson/baseToolsJson and runtime registry mutations so tools can
 * be registered at runtime (xiaozhi device MCP tools): the agent thread holds
 * it for the duration of each LLM call, registration re-renders under it. */
static pthread_mutex_t toolsMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static atomic_int running;
CrabRuntime* AgentLoopRuntime(void) {
    return &runtime;
}
int AgentLoopRefreshTools(void) {
    CrabToolCatalogView cat;
    CrabRuntimeGetToolCatalog(&runtime, &cat);
    pthread_mutex_lock(&toolsMu);
    int rc = LlmToolAdapterRenderOpenaiToolsJson(&cat, toolsJson, sizeof toolsJson) |
             LlmToolAdapterRenderOpenaiToolsJsonFiltered(&cat, 0, baseToolsJson, sizeof baseToolsJson);
    pthread_mutex_unlock(&toolsMu);
    return rc;
}
int AgentLoopRegisterTool(const CrabToolSpec* spec) {
    if (!spec || !spec->name || !spec->execute)
        return -1;
    pthread_mutex_lock(&toolsMu);
    int rc = CrabRegistryRegister(&runtime.registry, spec);
    pthread_mutex_unlock(&toolsMu);
    if (rc) {
        LogPrint("[kernel] dynamic tool register failed name=%s rc=%d", spec->name, rc);
        return rc == CRAB_ERROR_DUPLICATE_TOOL ? 0 : -1;
    }
    LogPrint("[kernel] dynamic tool registered name=%s", spec->name);
    return AgentLoopRefreshTools();
}
int AgentLoopInit(const CrabRuntimeConfig* rc, const LlmConfig* lc) {
    MessageBusInit();
    if (!rc || !rc->workspaceRoot || AgentSessionStoreConfigure(rc->workspaceRoot))
        return -1;
    AgentSessionStateInit();
    WorkingMemoryInit();
    SkillSupersetReset();
    char skillRoot[CRAB_RUNTIME_PATH_MAX];
    int skillPathLength =
        rc && rc->workspaceRoot
            ? snprintf(skillRoot, sizeof skillRoot, "%s/skills", rc->workspaceRoot)
            : -1;
    if (skillPathLength < 0 || (size_t) skillPathLength >= sizeof skillRoot)
        return -1;
    SkillLoadAllFrom(skillRoot);
    if (CrabRuntimeInit(&runtime, rc) || CrabRuntimeRegisterBuiltinTools(&runtime))
        return -1;
    CrabRuntimeStart(&runtime);
    CrabToolCatalogView cat;
    CrabRuntimeGetToolCatalog(&runtime, &cat);
    if (AgentLoopRefreshTools() || SkillRouterInit(&cat))
        return -1;
    if (LlmInit(lc))
        return -1;
    return 0;
}
static int append_tool_exchange(char* messages,
                                size_t z,
                                const LlmResponse* r,
                                char outputs[][AGENT_TOOL_OUTPUT_MAX]) {
    size_t cap = 4096;
    for (int i = 0; i < r->callCount; i++)
        cap += strlen(r->calls[i].input ? r->calls[i].input : "{}") + strlen(outputs[i]) + 512;
    char* obj = malloc(cap);
    if (!obj)
        return -1;
    LjBuf b;
    LjBufInit(&b, obj, cap);
    LjAppend(&b, "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[");
    for (int i = 0; i < r->callCount; i++) {
        if (i)
            LjAppend(&b, ",");
        LjAppend(&b, "{\"id\":");
        LjAppendJsonString(&b, r->calls[i].id);
        LjAppend(&b, ",\"type\":\"function\",\"function\":{\"name\":");
        LjAppendJsonString(&b, r->calls[i].name);
        LjAppend(&b, ",\"arguments\":");
        LjAppendJsonString(&b, r->calls[i].input ? r->calls[i].input : "{}");
        LjAppend(&b, "}}");
    }
    LjAppend(&b, "]}");
    int rc = b.failed ? -1 : array_append(messages, z, obj);
    free(obj);
    if (rc)
        return rc;
    for (int i = 0; i < r->callCount; i++) {
        cap = strlen(outputs[i]) * 6 + 256;
        obj = malloc(cap);
        if (!obj)
            return -1;
        LjBufInit(&b, obj, cap);
        LjAppend(&b, "{\"role\":\"tool\",\"tool_call_id\":");
        LjAppendJsonString(&b, r->calls[i].id);
        LjAppend(&b, ",\"content\":");
        LjAppendJsonString(&b, outputs[i]);
        LjAppend(&b, "}");
        rc = b.failed ? -1 : array_append(messages, z, obj);
        free(obj);
        if (rc)
            return rc;
    }
    return 0;
}
static int append_route_prompt(char* system, size_t z, const SkillRouterResult* route) {
    size_t used = strlen(system);
    int n;
    if (!route || route->action == SKILL_ROUTE_BASE) {
        n = snprintf(system + used,
                     z - used,
                     "\nSkill Router decision: no specialized skill is required. Handle this "
                     "request as the base agent. Do not start, infer, or load a skill workflow "
                     "in this turn.\n");
        if (n < 0 || (size_t) n >= z - used)
            return -1;
        used += (size_t) n;
    } else {
        n = snprintf(system + used,
                     z - used,
                     "\nSkill Router decision: select `%s`. Call `skill_read` for exactly this "
                     "Skill before following its workflow. Do not select or load another Skill.\n",
                     route->skillName);
    }
    return n < 0 || (size_t) n >= z - used ? -1 : 0;
}
static char* run_round(const char* input,
                       const char* sessionId,
                       const char* requestId,
                       const SkillRouterResult* route,
                       AgentTraceScope* scope,
                       const LiteMsg* request) {
    size_t cap = AGENT_MAX_CONVERSATION_BYTES + 32768;
    char* messages = calloc(cap, 1);
    if (!messages)
        return NULL;
    snprintf(messages, cap, "%s", AgentSessionStateLoad());
    if (MessageTreeAppendUser(messages, cap, input)) {
        free(messages);
        return strdup("ERROR: conversation is too large");
    }
    char system[16384];
    if (ContextBuildSystemPromptEx(system, sizeof system, 1) ||
        append_route_prompt(system, sizeof system, route)) {
        free(messages);
        return strdup("ERROR: system prompt is too large");
    }
    CrabToolCatalogView cat;
    CrabRuntimeGetToolCatalog(&runtime, &cat);
    char* answer = NULL;
    SkillHandlerTurnState turn = {0};
    int toolCalls = 0;
    int toolCallLimit = request->priority >= LITE_PRIORITY_HIGH ? 24 : 16;
    for (int iter = 0; iter < LITE_AGENT_MAX_TOOL_ITER; iter++) {
        if (RequestShouldStop(request)) {
            answer = strdup("ERROR: request cancelled or timed out");
            break;
        }
        LlmResponse resp = {0};
        if (SkillSupersetGetActive())
            SkillSupersetIncrementLlmCalls();
        pthread_mutex_lock(&toolsMu);
        const char* routeTools =
            route && (route->action == SKILL_ROUTE_SELECT || route->action == SKILL_ROUTE_RESUME)
                ? toolsJson
                : baseToolsJson;
        int rc = LlmChatToolsEx(system, messages, routeTools, &resp, -1);
        pthread_mutex_unlock(&toolsMu);
        if (rc) {
            const LlmConfig* llmCfg = LlmGetConfig();
            AgentTraceLogLlm(scope,
                             llmCfg ? llmCfg->model : "",
                             iter + 1,
                             "error",
                             0,
                             "[]",
                             "[]",
                             system,
                             messages,
                             "LLM request failed",
                             0,
                             0,
                             0,
                             rc);
            LlmResponseClear(&resp);
            char failure[96];
            snprintf(failure, sizeof failure, "ERROR: LLM request failed (%d)", rc);
            answer = strdup(failure);
            break;
        }
        scope->totalPromptTokens += resp.promptTokens;
        scope->totalCompletionTokens += resp.completionTokens;
        scope->llmCallCount++;
        char llmToolNames[1024] = "[]";
        char llmToolCalls[65536] = "[]";
        SkillHandlerBuildToolNamesJson(&resp, llmToolNames, sizeof llmToolNames);
        SkillHandlerBuildToolCallsJson(&resp, llmToolCalls, sizeof llmToolCalls);
        const LlmConfig* llmCfg = LlmGetConfig();
        AgentTraceLogLlm(scope,
                         llmCfg ? llmCfg->model : "",
                         iter + 1,
                         "ok",
                         resp.toolUse,
                         llmToolNames,
                         llmToolCalls,
                         system,
                         messages,
                         resp.text,
                         resp.promptTokens,
                         resp.completionTokens,
                         resp.reasoningTokens,
                         0);
        if (!resp.toolUse) {
            answer = SkillHandlerExtractFinalText(resp.text, resp.textLen);
            MessageTreeAppendAssistant(messages, cap, resp.text);
            const SkillStateSuperset* activeSkill = SkillSupersetGetActive();
            if (activeSkill) {
                SkillSuspendQueuePush(activeSkill);
                SkillHandlerSuspend(scope, sessionId, requestId);
            }
            LlmResponseClear(&resp);
            break;
        }
        SkillHandlerOnSkillStart(scope, &resp, sessionId, requestId);
        char outputs[LLM_MAX_TOOL_CALLS][AGENT_TOOL_OUTPUT_MAX];
        memset(outputs, 0, sizeof outputs);
        for (int i = 0; i < resp.callCount; i++) {
            if (RequestShouldStop(request)) {
                snprintf(outputs[i], sizeof outputs[i], "tool cancelled: request deadline reached");
                continue;
            }
            if (toolCalls >= toolCallLimit) {
                snprintf(outputs[i], sizeof outputs[i], "tool limit reached: maximum %d calls", toolCallLimit);
                continue;
            }
            toolCalls++;
            CrabToolCall call;
            CrabToolResponse tr;
            char err[256];
            if (LlmToolAdapterParseCall(&cat,
                                        resp.calls[i].id,
                                        resp.calls[i].name,
                                        resp.calls[i].input,
                                        &call,
                                        err,
                                        sizeof err))
                snprintf(outputs[i], sizeof outputs[i], "tool parse error: %s", err);
            else {
                CrabRuntimeCallTool(&runtime, &call, &tr);
                size_t copy = strnlen(tr.content, sizeof tr.content);
                if (copy >= sizeof outputs[i])
                    copy = sizeof outputs[i] - 1;
                memcpy(outputs[i], tr.content, copy);
                outputs[i][copy] = 0;
                AgentTraceLogTool(scope,
                                  NULL,
                                  call.toolName,
                                  resp.calls[i].input,
                                  outputs[i],
                                  tr.toolExitCode,
                                  tr.success ? "ok" : "error");
                SkillHandlerOnToolResult(scope, &call, outputs[i], sessionId, requestId, &turn);
                LlmToolAdapterFreeCall(&call);
            }
        }
        if (append_tool_exchange(messages, cap, &resp, outputs)) {
            answer = strdup("ERROR: conversation is too large");
            LlmResponseClear(&resp);
            break;
        }
        LlmResponseClear(&resp);
    }
    if (!answer)
        answer = strdup("Reached maximum tool iterations before the agent could finish.");
    if (SkillSupersetGetActive()) {
        SkillSuspendQueuePush(SkillSupersetGetActive());
        SkillHandlerSuspend(scope, sessionId, requestId);
    }
    AgentSessionStateSave(messages);
    free(messages);
    return answer;
}

static void finish_request(const LiteMsg* request, char* content) {
    if (request->replyMode == LITE_REPLY_ACK_ONLY) {
        free(content);
        return;
    }
    LiteMsg response = {0};
    response.type = LITE_MSG_CHAT;
    response.replyMode = request->replyMode;
    snprintf(response.requestId, sizeof response.requestId, "%s", request->requestId);
    snprintf(response.sessionId, sizeof response.sessionId, "%s", request->sessionId);
    response.content = content ? content : strdup("ERROR: internal allocation failure");
    if (!response.content) {
        RequestCancel(request->requestId);
        return;
    }
    if (!strncmp(response.content,
                 "ERROR: LLM request failed",
                 strlen("ERROR: LLM request failed")) ||
        !strncmp(response.content,
                 "ERROR: request cancelled or timed out",
                 strlen("ERROR: request cancelled or timed out")) ||
        !strncmp(response.content,
                 "ERROR: session unavailable",
                 strlen("ERROR: session unavailable")) ||
        !strncmp(response.content,
                 "ERROR: session capacity exhausted",
                 strlen("ERROR: session capacity exhausted")) ||
        !strncmp(response.content,
                 "ERROR: internal allocation failure",
                 strlen("ERROR: internal allocation failure")))
        response.completionStatus = LITE_COMPLETION_RETRYABLE;
    else if (!strncmp(response.content, "ERROR:", 6))
        response.completionStatus = LITE_COMPLETION_PERMANENT;
    else
        response.completionStatus = LITE_COMPLETION_HANDLED;
    if (RequestComplete(&response))
        free(response.content);
}

static void* agent_main(void* x) {
    (void) x;
    while (atomic_load(&running)) {
        LiteMsg m = {0};
        MessageBusPopInbound(&m);
        if (!atomic_load(&running)) {
            LiteMsgClear(&m);
            break;
        }
        if (m.type == LITE_MSG_SESSION_OPEN) {
            /* A transport connection is not a durable Session. Create the
             * Session lazily when its first chat message arrives. */
        } else if (m.type == LITE_MSG_SESSION_CLOSE)
            AgentSessionStateReleaseById(m.sessionId);
        else if (m.type == LITE_MSG_CHAT) {
            if (RequestShouldStop(&m)) {
                finish_request(&m, strdup("ERROR: request cancelled or timed out"));
                LiteMsgClear(&m);
                continue;
            }
            SessionHandle session = {0};
            if (AgentSessionStateAcquire(m.sessionId, 1, &session) ||
                AgentSessionStateUse(&session)) {
                finish_request(&m, strdup("ERROR: session capacity exhausted"));
                LiteMsgClear(&m);
                continue;
            }
            if (AgentSessionStateBindUser(m.userId)) {
                finish_request(&m, strdup("ERROR: session is owned by another user"));
                AgentSessionStateRelease(&session);
                LiteMsgClear(&m);
                continue;
            }
            AgentTraceScope trace;
            AgentTraceStart(&trace, m.userId, m.sessionId, m.content);
            LlmRequestScopeBegin(m.deadlineMs, m.priority >= LITE_PRIORITY_HIGH ? 12 : 8);
            SkillRouterResult route = SkillRouterRun(AgentSessionStateLoad(),
                                                     m.content,
                                                     m.replyToRunId,
                                                     m.replyToInterruptId,
                                                     m.correlationToken,
                                                     &trace);
            char routeEvent[1024];
            snprintf(routeEvent,
                     sizeof routeEvent,
                     "event=skill.candidates.resolved session_id=%s action=%d skill=%s "
                     "confidence=%.3f reason=%s source=%s evidence=%s",
                     m.sessionId,
                     route.action,
                     route.skillName,
                     route.confidence,
                     route.reasonCode,
                     route.source,
                     route.evidence);
            AgentTraceLogMessage("INFO", "skill_router", routeEvent);
            AgentSessionStateSetSkillRoute(route.skillName,
                                           route.action == SKILL_ROUTE_SELECT ||
                                               route.action == SKILL_ROUTE_RESUME);
            if (route.action == SKILL_ROUTE_RESUME)
                SkillRouterRecover(
                    route.skillName, route.skillRunId, &trace, m.sessionId, m.requestId);
            char* ans =
                route.action == SKILL_ROUTE_ERROR
                    ? strdup(route.question)
                    : run_round(
                          m.content ? m.content : "", m.sessionId, m.requestId, &route, &trace, &m);
            AgentTraceFinish(&trace,
                             route.action != SKILL_ROUTE_ERROR && ans && strncmp(ans, "ERROR:", 6)
                                 ? "ok"
                                 : "error",
                             ans);
            LlmRequestScopeEnd();
            finish_request(&m, ans);
            AgentSessionStateRelease(&session);
        }
        LiteMsgClear(&m);
    }
    return NULL;
}
int AgentLoopStart(void) {
    if (atomic_load(&running))
        return 0;
    atomic_store(&running, 1);
    if (pthread_create(&thread, NULL, agent_main, NULL)) {
        atomic_store(&running, 0);
        return -1;
    }
    return 0;
}
void AgentLoopStop(void) {
    if (!atomic_load(&running))
        return;
    LlmCancelActiveRequests();
    atomic_store(&running, 0);
    LiteMsg m = {.type = LITE_MSG_EXCEPTION};
    m.content = strdup("stop");
    MessageBusPushInbound(&m);
    pthread_join(thread, NULL);
    CrabRuntimeStop(&runtime);
}
