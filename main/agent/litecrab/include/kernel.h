#ifndef LITECRAB_KERNEL_H
#define LITECRAB_KERNEL_H
#include "litecrab/observability.h"
#include "litecrab/runtime.h"
#include "litecrab/session.h"
#include "litecrab/working_memory.h"

#include <stddef.h>
#define AGENT_TOOL_OUTPUT_MAX (16 * 1024)
#define LLM_MAX_TOOL_CALLS 8
#define LLM_RESPONSE_TEXT_MAX (64 * 1024)

typedef struct {
    char id[64], name[64];
    char* input;
    size_t inputLen, inputCapacity;
} LlmToolCall;
typedef struct {
    char text[LLM_RESPONSE_TEXT_MAX];
    size_t textLen;
    LlmToolCall calls[LLM_MAX_TOOL_CALLS];
    int callCount, toolUse, promptTokens, completionTokens, totalTokens, reasoningTokens;
} LlmResponse;
typedef struct {
    const char* baseUrl;
    char host[256];
    int port;
    char path[512];
    int useTls;
    const char *apiKey, *model;
    int maxTokens;
    double temperature;
    int stream;
    const char* reasoningEffort;
    int timeoutMs;
} LlmConfig;
int LlmInit(const LlmConfig*);
void LlmRequestScopeBegin(long long deadlineMs, int maxCalls);
void LlmRequestScopeEnd(void);
void LlmCancelActiveRequests(void);
const LlmConfig* LlmGetConfig(void);
int LlmBuildChatToolsRequestJson(const char*, const char*, int, char*, size_t);
int LlmChatToolsEx(const char* systemPrompt,
                   const char* messagesJson,
                   const char* toolsJson,
                   LlmResponse*,
                   int stream);
int LlmParseResponseBody(const char*, int stream, LlmResponse*);
void LlmResponseClear(LlmResponse*);
int LlmToolAdapterRenderOpenaiToolsJson(const CrabToolCatalogView*, char*, size_t);
int LlmToolAdapterRenderOpenaiToolsJsonFiltered(const CrabToolCatalogView*,
                                                int includeSkillTools,
                                                char*,
                                                size_t);
int LlmToolAdapterParseCall(const CrabToolCatalogView*,
                            const char*,
                            const char*,
                            const char*,
                            CrabToolCall*,
                            char*,
                            size_t);
void LlmToolAdapterFreeCall(CrabToolCall*);
int JsonExtractStringField(const char*, const char*, char*, size_t);
int JsonExtractSkillPath(const char*, char*, size_t);
int TextExtractMarkerInt(const char*, const char*, int*);
int MessageTreeAppendAssistant(char*, size_t, const char*);
int MessageTreeAppendUser(char*, size_t, const char*);

int ContextBuildSystemPromptEx(char*, size_t, int);

typedef enum {
    SKILL_EXEC_PHASE_INACTIVE = 0,
    SKILL_EXEC_PHASE_RUNNING,
    SKILL_EXEC_PHASE_WAITING_INPUT,
    SKILL_EXEC_PHASE_RESUMING,
    SKILL_EXEC_PHASE_SUCCESS,
    SKILL_EXEC_PHASE_FAILED,
    SKILL_EXEC_PHASE_PARTIAL
} SkillExecPhase;
typedef struct {
    char skillName[128], skillRunId[64], scriptsDir[256];
} SkillBasicInfo;
typedef struct {
    int active;
    SkillExecPhase phase;
    int stepCount, llmCalls;
    long long createdTimeMs, lastActiveTimeMs;
    char lastOutcome[16];
} SkillExecState;
typedef struct {
    char skillSpanId[64];
    int totalPromptTokens, totalCompletionTokens;
} SkillTraceInfo;
typedef struct {
    SkillBasicInfo basic;
    SkillExecState exec;
    SkillTraceInfo trace;
} SkillStateSuperset;
typedef struct {
    SkillBasicInfo basic;
    SkillExecState exec;
    char sessionId[64];
    char interruptionId[64], correlationToken[64];
} SkillSuspendEntry;
typedef struct {
    char name[128], prompt[2048], path[512], packageRoot[1024], scriptsDir[1024];
    char definitionHash[65];
} SkillEntry;
/* Embedded skill definition (compile-time replacement for the per-skill
 * SKILL.md files). Keeps the skill subsystem free of filesystem access for
 * the ESP32 port. */
typedef struct {
    const char* name;
    const char* description;  /* router selection metadata */
    const char* body;         /* SKILL.md body equivalent, loaded via skill_read */
} RobotSkillDef;
const RobotSkillDef* RobotSkillsAll(int* count);
const SkillStateSuperset* SkillSupersetGetActive(void);
int SkillSupersetActivate(const char*, const char*);
int SkillSupersetFinish(SkillExecPhase, const char*);
void SkillSupersetReset(void);
void SkillSupersetOnSessionClose(void);
int SkillSupersetUpdateExecPhase(SkillExecPhase, const char*);
int SkillSupersetIncrementStep(int*);
int SkillSupersetIncrementLlmCalls(void);
const char* SkillExecPhaseToString(SkillExecPhase);
int SkillSuspendQueuePush(const SkillStateSuperset*);
int SkillSuspendQueuePopByName(const char*, SkillSuspendEntry*);
void SkillSuspendQueueClear(void);
void SkillSuspendQueueClearSession(const char*);
int SkillSuspendQueueGetCount(void);
int SkillSuspendQueueBuildSimpleContext(char*, size_t);
int SkillSupersetReactivate(const SkillSuspendEntry*);
int SkillLoadAll(void);
int SkillLoadAllFrom(const char*);
int SkillGetEntries(SkillEntry*, int);
const char* SkillGetCanonicalRoot(void);
int SkillReadContent(const char*, const char*, char**, size_t*, int*);

typedef enum {
    SKILL_RUN_CREATED = 0,
    SKILL_RUN_READY,
    SKILL_RUN_RUNNING,
    SKILL_RUN_WAITING_INPUT,
    SKILL_RUN_SUSPENDED,
    SKILL_RUN_COMPLETED,
    SKILL_RUN_FAILED,
    SKILL_RUN_CANCELLED
} SkillRunStatus;
typedef struct {
    char skillRunId[64], sessionId[64], skillName[128], definitionHash[65];
    char interruptionId[64], correlationToken[64];
    SkillRunStatus status;
    unsigned long revision;
    int stepCount, llmCalls;
    long long createdTimeMs, updatedTimeMs;
    char reason[64];
} SkillRunSnapshot;
int SkillRunGetSnapshot(const char*, const char*, SkillRunSnapshot*);
int SkillRunListPending(const char*, SkillRunSnapshot*, int);
const char* SkillRunStatusToString(SkillRunStatus);
typedef struct {
    int relatedToolSeen, hasLastExitCode, lastExitCode;
} SkillHandlerTurnState;
void SkillHandlerOnSkillStart(AgentTraceScope*, const LlmResponse*, const char*, const char*);
void SkillHandlerOnToolResult(AgentTraceScope*,
                              const CrabToolCall*,
                              const char*,
                              const char*,
                              const char*,
                              SkillHandlerTurnState*);
void SkillHandlerSuspend(AgentTraceScope*, const char*, const char*);
void SkillHandlerFinish(AgentTraceScope*, const char*, const char*, SkillExecPhase);
char* SkillHandlerExtractFinalText(const char*, size_t);
int SkillHandlerBuildToolNamesJson(const LlmResponse*, char*, size_t);
int SkillHandlerBuildToolCallsJson(const LlmResponse*, char*, size_t);
typedef enum {
    SKILL_RULE_UNRESOLVED = 0,
    SKILL_RULE_BASE,
    SKILL_RULE_SELECT,
    SKILL_RULE_RESUME,
    SKILL_RULE_PROTOCOL_ERROR
} SkillRuleDecision;
typedef enum {
    SKILL_ROUTE_BASE = 0,
    SKILL_ROUTE_SELECT,
    SKILL_ROUTE_RESUME,
    SKILL_ROUTE_ERROR
} SkillRouteAction;
#define SKILL_ROUTE_NONE SKILL_ROUTE_BASE
typedef struct {
    SkillRouteAction action;
    char skillName[128], skillRunId[64], interruptionId[64];
    double confidence;
    char reasonCode[64], question[256], evidence[256], source[16];
} SkillRouterResult;
int SkillRouterInit(const CrabToolCatalogView*);
SkillRouterResult
SkillRouterRun(const char*, const char*, const char*, const char*, const char*, AgentTraceScope*);
int SkillRouterRecover(const char*, const char*, AgentTraceScope*, const char*, const char*);

int AgentLoopInit(const CrabRuntimeConfig*, const LlmConfig*);
int AgentLoopStart(void);
void AgentLoopStop(void);
CrabRuntime* AgentLoopRuntime(void);
/* Runtime tool registration (e.g. device MCP tools discovered when a xiaozhi
 * device connects): registers into the live registry and re-renders the
 * cached OpenAI tools JSON so the LLM advertises the tool from the next
 * chat request on. Duplicate names are treated as success. */
int AgentLoopRegisterTool(const CrabToolSpec* spec);
int AgentLoopRefreshTools(void);
#endif
