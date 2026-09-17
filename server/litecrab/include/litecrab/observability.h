#ifndef LITECRAB_OBSERVABILITY_H
#define LITECRAB_OBSERVABILITY_H
#include <stdio.h>
typedef struct {
    char traceId[64], rootSpanId[64];
    long long startTimeMs;
    unsigned int systemPromptHash;
    int systemPromptRecorded, lastMessageCount;
    char skillSpanId[64];
    int totalPromptTokens, totalCompletionTokens, llmCallCount, toolCallCount;
} AgentTraceScope;
typedef struct {
    char spanId[64], traceId[64], parentSpanId[64], type[32], name[96];
    long long startTimeMs;
} AgentTraceSpan;
typedef enum { AGENT_TRACE_LLM_FULL, AGENT_TRACE_LLM_INCREMENTAL } AgentTraceLlmMode;
typedef enum { AGENT_TRACE_ANOMALY_WARN, AGENT_TRACE_ANOMALY_ERROR } AgentTraceAnomalyLevel;
extern FILE* g_logFp;
extern char g_logFilename[256];
int InitLog(void);
int InitLogWithDir(const char*);
void CloseLog(void);
void LogPrint(const char*, ...);
int AgentTraceInit(const char*);
void AgentTraceClose(void);
const char* AgentTraceGetFilename(void);
int AgentTraceStart(AgentTraceScope*, const char*, const char*, const char*);
void AgentTraceFinish(AgentTraceScope*, const char*, const char*);
int AgentTraceStartSpan(AgentTraceScope*,
                        AgentTraceSpan*,
                        const char*,
                        const char*,
                        const char*,
                        const char*,
                        const char*);
void AgentTraceEndSpan(AgentTraceSpan*, const char*, const char*);
void AgentTraceLogTool(
    AgentTraceScope*, AgentTraceSpan*, const char*, const char*, const char*, int, const char*);
void AgentTraceLogLlm(AgentTraceScope*,
                      const char* model,
                      int iteration,
                      const char* status,
                      int toolUse,
                      const char* toolNamesJson,
                      const char* toolCallsJson,
                      const char* systemPrompt,
                      const char* messages,
                      const char* out,
                      int promptTokens,
                      int completionTokens,
                      int reasoningTokens,
                      int errorCode);
void AgentTraceSetLogLlmInput(int enable);
void AgentTraceLogAnomaly(
    AgentTraceScope*, const char*, const char*, AgentTraceAnomalyLevel, const char*);
void AgentTraceLogMessage(const char*, const char*, const char*);
#endif
