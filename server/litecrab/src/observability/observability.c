#include "litecrab/observability.h"

#include "litecrab/json.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

FILE* g_logFp;
char g_logFilename[256];
static FILE* traceFp;
static char traceName[512];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long ids;
static unsigned int logPending, tracePending;
static long long logLastFlushMs, traceLastFlushMs;
#define LOG_FILE_MAX_BYTES (2L * 1024L * 1024L)
#define LOG_FILE_GENERATIONS 4
/* When non-zero, AgentTraceLogLlm records the messages array sent to the LLM
 * as the "input" field. Toggled via AgentTraceSetLogLlmInput or auto-enabled
 * from the LITECRAB_TRACE_LOG_LLM_INPUT environment variable at AgentTraceInit
 * time. Defaults to off to keep trace files small and avoid leaking prompts. */
static int g_traceLogLlmInput = 0;
static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (long long) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void make_id(char* out, size_t z, const char* prefix) {
    pthread_mutex_lock(&mu);
    unsigned long n = ++ids;
    pthread_mutex_unlock(&mu);
    snprintf(out, z, "%s-%lld-%lu", prefix, now_ms(), n);
}
static int mkdirs(const char* p) {
    char b[512];
    snprintf(b, sizeof b, "%s", p);
    for (char* s = b + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir(b, 0775) && errno != EEXIST)
                return -1;
            *s = '/';
        }
    return mkdir(b, 0775) && errno != EEXIST ? -1 : 0;
}
static void rotate_if_needed(FILE** file, const char* path) {
    if (!file || !*file || !path)
        return;
    long position = ftell(*file);
    if (position < LOG_FILE_MAX_BYTES)
        return;
    fflush(*file);
    fclose(*file);
    *file = NULL;
    char from[640], to[640];
    snprintf(to, sizeof to, "%s.%d", path, LOG_FILE_GENERATIONS - 1);
    unlink(to);
    for (int generation = LOG_FILE_GENERATIONS - 2; generation >= 1; generation--) {
        snprintf(from, sizeof from, "%s.%d", path, generation);
        snprintf(to, sizeof to, "%s.%d", path, generation + 1);
        rename(from, to);
    }
    snprintf(to, sizeof to, "%s.1", path);
    rename(path, to);
    *file = fopen(path, "a");
}
typedef struct {
    char path[640];
    time_t modified;
} LogFile;
static int newest_first(const void* left, const void* right) {
    const LogFile *a = left, *b = right;
    return a->modified > b->modified ? -1 : a->modified < b->modified ? 1 : 0;
}
static void prune_trace_files(const char* dir) {
    DIR* directory = opendir(dir);
    if (!directory)
        return;
    LogFile files[64];
    size_t count = 0;
    struct dirent* entry;
    while ((entry = readdir(directory))) {
        if (strncmp(entry->d_name, "agent_trace_", 12))
            continue;
        char path[640];
        int n = snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
        struct stat status;
        if (n < 0 || (size_t) n >= sizeof path || stat(path, &status) ||
            !S_ISREG(status.st_mode))
            continue;
        if (count < sizeof files / sizeof files[0]) {
            snprintf(files[count].path, sizeof files[count].path, "%s", path);
            files[count].modified = status.st_mtime;
            count++;
        } else {
            unlink(path);
        }
    }
    closedir(directory);
    qsort(files, count, sizeof files[0], newest_first);
    for (size_t i = LOG_FILE_GENERATIONS - 1; i < count; i++)
        unlink(files[i].path);
}
static void field(LjBuf* b, const char* k, const char* v, int comma) {
    if (comma)
        LjAppend(b, ",");
    LjAppendJsonString(b, k);
    LjAppend(b, ":");
    LjAppendJsonString(b, v ? v : "");
}
static void emit_line(const char* type, const char* fields) {
    if (!traceFp)
        return;
    pthread_mutex_lock(&mu);
    rotate_if_needed(&traceFp, traceName);
    if (!traceFp) {
        pthread_mutex_unlock(&mu);
        return;
    }
    fprintf(traceFp,
            "{\"type\":\"%s\",\"timeMs\":%lld%s%s}\n",
            type,
            now_ms(),
            fields && *fields ? "," : "",
            fields ? fields : "");
    tracePending++;
    long long current = now_ms();
    if (tracePending >= 16 || current - traceLastFlushMs >= 1000 ||
        !strcmp(type, "anomaly") || !strcmp(type, "trace_end")) {
        fflush(traceFp);
        tracePending = 0;
        traceLastFlushMs = current;
    }
    pthread_mutex_unlock(&mu);
}
int AgentTraceInit(const char* dir) {
    if (!dir)
        dir = "logs";
    mkdirs(dir);
    prune_trace_files(dir);
    /* Auto-enable LLM input logging when the user opts in via env. Allowed
     * values: "1", "true", "yes" (case-insensitive). Anything else is off. */
    const char* env = getenv("LITECRAB_TRACE_LOG_LLM_INPUT");
    if (env) {
        int on = !strcasecmp(env, "1") || !strcasecmp(env, "true") ||
                 !strcasecmp(env, "yes") || !strcasecmp(env, "on");
        g_traceLogLlmInput = on ? 1 : 0;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);
    for (int i = 0; i < 1000; i++) {
        snprintf(traceName,
                 sizeof traceName,
                 i ? "%s/agent_trace_%s_%03d.jsonl" : "%s/agent_trace_%s.jsonl",
                 dir,
                 stamp,
                 i);
        if (access(traceName, F_OK))
            break;
    }
    traceFp = fopen(traceName, "a");
    tracePending = 0;
    traceLastFlushMs = now_ms();
    return traceFp ? 0 : -1;
}
void AgentTraceClose(void) {
    pthread_mutex_lock(&mu);
    if (traceFp)
        fclose(traceFp);
    traceFp = NULL;
    pthread_mutex_unlock(&mu);
}
const char* AgentTraceGetFilename(void) {
    return traceName;
}
int InitLogWithDir(const char* dir) {
    if (!dir)
        dir = "logs";
    mkdirs(dir);
    snprintf(g_logFilename, sizeof g_logFilename, "%s/litecrab.log", dir);
    g_logFp = fopen(g_logFilename, "a");
    if (!g_logFp)
        return -1;
    pthread_mutex_lock(&mu);
    rotate_if_needed(&g_logFp, g_logFilename);
    logPending = 0;
    logLastFlushMs = now_ms();
    pthread_mutex_unlock(&mu);
    if (!g_logFp)
        return -1;
    if (AgentTraceInit(dir)) {
        fclose(g_logFp);
        g_logFp = NULL;
        return -1;
    }
    return 0;
}
int InitLog(void) {
    return InitLogWithDir("logs");
}
void CloseLog(void) {
    pthread_mutex_lock(&mu);
    if (g_logFp)
        fclose(g_logFp);
    g_logFp = NULL;
    pthread_mutex_unlock(&mu);
    AgentTraceClose();
}
void AgentTraceLogMessage(const char* level, const char* component, const char* message) {
    char b[8192];
    LjBuf j;
    LjBufInit(&j, b, sizeof b);
    field(&j, "level", level, 0);
    field(&j, "component", component, 1);
    char m[2049];
    snprintf(m, sizeof m, "%s", message ? message : "");
    field(&j, "message", m, 1);
    emit_line("log", b);
}
void LogPrint(const char* fmt, ...) {
    if (!fmt)
        return;
    char b[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    size_t n = strlen(b);
    while (n && (b[n - 1] == '\r' || b[n - 1] == '\n'))
        b[--n] = 0;
    pthread_mutex_lock(&mu);
    if (g_logFp) {
        rotate_if_needed(&g_logFp, g_logFilename);
        if (!g_logFp) {
            pthread_mutex_unlock(&mu);
            return;
        }
        fprintf(g_logFp, "%s\n", b);
        logPending++;
        long long current = now_ms();
        if (logPending >= 16 || current - logLastFlushMs >= 1000 || strstr(b, "ERROR") ||
            strstr(b, "failed") || strstr(b, "WARN")) {
            fflush(g_logFp);
            logPending = 0;
            logLastFlushMs = current;
        }
    }
    pthread_mutex_unlock(&mu);
    char comp[64] = "general";
    if (b[0] == '[') {
        char* e = strchr(b, ']');
        if (e && (size_t) (e - b - 1) < sizeof comp) {
            memcpy(comp, b + 1, (size_t) (e - b - 1));
            comp[e - b - 1] = 0;
        }
    }
    const char* lev = strstr(b, "ERROR") || strstr(b, "failed")   ? "error"
                      : strstr(b, "WARN") || strstr(b, "WARNING") ? "warn"
                                                                  : "info";
    AgentTraceLogMessage(lev, comp, b);
}
int AgentTraceStart(AgentTraceScope* s, const char* user, const char* session, const char* input) {
    if (!s)
        return -1;
    memset(s, 0, sizeof *s);
    make_id(s->traceId, sizeof s->traceId, "trace");
    make_id(s->rootSpanId, sizeof s->rootSpanId, "span");
    s->startTimeMs = now_ms();
    char b[8192];
    LjBuf j;
    LjBufInit(&j, b, sizeof b);
    field(&j, "traceId", s->traceId, 0);
    field(&j, "rootSpanId", s->rootSpanId, 1);
    field(&j, "userId", user, 1);
    field(&j, "sessionId", session, 1);
    field(&j, "input", input, 1);
    emit_line("trace_start", b);
    return 0;
}
void AgentTraceFinish(AgentTraceScope* s, const char* status, const char* out) {
    if (!s)
        return;
    char b[8192];
    LjBuf j;
    LjBufInit(&j, b, sizeof b);
    field(&j, "traceId", s->traceId, 0);
    field(&j, "status", status, 1);
    field(&j, "finalOutput", out, 1);
    LjAppend(&j,
             ",\"durationMs\":%lld,\"promptTokens\":%d,\"completionTokens\":%d,\"llmCalls\":%d,"
             "\"toolCalls\":%d",
             now_ms() - s->startTimeMs,
             s->totalPromptTokens,
             s->totalCompletionTokens,
             s->llmCallCount,
             s->toolCallCount);
    emit_line("trace_end", b);
}
int AgentTraceStartSpan(AgentTraceScope* s,
                        AgentTraceSpan* sp,
                        const char* parent,
                        const char* type,
                        const char* name,
                        const char* input,
                        const char* meta) {
    if (!s || !sp)
        return -1;
    memset(sp, 0, sizeof *sp);
    make_id(sp->spanId, sizeof sp->spanId, "span");
    snprintf(sp->traceId, sizeof sp->traceId, "%s", s->traceId);
    snprintf(sp->parentSpanId,
             sizeof sp->parentSpanId,
             "%s",
             parent && *parent ? parent : s->rootSpanId);
    snprintf(sp->type, sizeof sp->type, "%s", type ? type : "");
    snprintf(sp->name, sizeof sp->name, "%s", name ? name : "");
    sp->startTimeMs = now_ms();
    char b[8192];
    LjBuf j;
    LjBufInit(&j, b, sizeof b);
    field(&j, "traceId", sp->traceId, 0);
    field(&j, "spanId", sp->spanId, 1);
    field(&j, "parentSpanId", sp->parentSpanId, 1);
    field(&j, "spanType", sp->type, 1);
    field(&j, "name", sp->name, 1);
    field(&j, "input", input, 1);
    field(&j, "metadata", meta, 1);
    emit_line("span_start", b);
    return 0;
}
void AgentTraceEndSpan(AgentTraceSpan* sp, const char* status, const char* out) {
    if (!sp)
        return;
    char b[8192];
    LjBuf j;
    LjBufInit(&j, b, sizeof b);
    field(&j, "traceId", sp->traceId, 0);
    field(&j, "spanId", sp->spanId, 1);
    field(&j, "status", status, 1);
    field(&j, "output", out, 1);
    LjAppend(&j, ",\"durationMs\":%lld", now_ms() - sp->startTimeMs);
    emit_line("span_end", b);
}
void AgentTraceLogAnomaly(AgentTraceScope* s,
                          const char* parent,
                          const char* cat,
                          AgentTraceAnomalyLevel level,
                          const char* detail) {
    char b[8192];
    LjBuf j;
    LjBufInit(&j, b, sizeof b);
    field(&j, "traceId", s ? s->traceId : "", 0);
    field(&j, "parentSpanId", parent, 1);
    field(&j, "category", cat, 1);
    field(&j, "level", level == AGENT_TRACE_ANOMALY_ERROR ? "error" : "warn", 1);
    field(&j, "detail", detail, 1);
    emit_line("anomaly", b);
}
void AgentTraceLogTool(AgentTraceScope* s,
                       AgentTraceSpan* sp,
                       const char* name,
                       const char* input,
                       const char* out,
                       int code,
                       const char* status) {
    if (s)
        s->toolCallCount++;
    char clipped[16385];
    snprintf(clipped, sizeof clipped, "%s", out ? out : "");
    char b[32768];
    LjBuf j;
    LjBufInit(&j, b, sizeof b);
    field(&j, "traceId", s ? s->traceId : "", 0);
    field(&j, "spanId", sp ? sp->spanId : "", 1);
    field(&j, "toolName", name, 1);
    field(&j, "input", input, 1);
    field(&j, "output", clipped, 1);
    LjAppend(&j, ",\"exitCode\":%d", code);
    field(&j, "status", status, 1);
    emit_line("tool", b);
    if (code)
        AgentTraceLogAnomaly(
            s, sp ? sp->spanId : NULL, "tool_error", AGENT_TRACE_ANOMALY_ERROR, out);
}
void AgentTraceSetLogLlmInput(int enable) {
    g_traceLogLlmInput = enable ? 1 : 0;
}
void AgentTraceLogLlm(AgentTraceScope* s,
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
                      int errorCode) {
    char clipped[4097];
    snprintf(clipped, sizeof clipped, "%s", out ? out : "");
    /* 96KB so the full conversation (system ~16KB + messages ~24KB +
     * JSON overhead) fits without the LjBuf truncating mid-array. */
    char b[98304];
    LjBuf j;
    LjBufInit(&j, b, sizeof b);
    field(&j, "traceId", s ? s->traceId : "", 0);
    field(&j, "model", model, 1);
    LjAppend(&j, ",\"iteration\":%d", iteration);
    field(&j, "status", status, 1);
    LjAppend(&j, ",\"toolUse\":%s", toolUse ? "true" : "false");
    LjAppend(&j,
             ",\"toolNames\":%s",
             toolNamesJson && !LjValidate(toolNamesJson, LJ_ARRAY) ? toolNamesJson : "[]");
    LjAppend(&j,
             ",\"toolCalls\":%s",
             toolCallsJson && !LjValidate(toolCallsJson, LJ_ARRAY) ? toolCallsJson : "[]");
    if (g_traceLogLlmInput) {
        /* Reconstruct the same messages array that request_json() sends to
         * the LLM: [{"role":"system","content":"<system>"},...messages].
         * This captures the system prompt, conversation history, current
         * user request, prior assistant replies and tool results — i.e. the
         * complete payload submitted to the model. */
        int hasSystem = systemPrompt && *systemPrompt;
        int hasMessages = messages && *messages && !LjValidate(messages, LJ_ARRAY);
        LjAppend(&j, ",\"input\":");
        if (!hasSystem && !hasMessages) {
            LjAppend(&j, "[]");
        } else {
            LjAppend(&j, "[");
            if (hasSystem) {
                LjAppend(&j, "{\"role\":\"system\",\"content\":");
                LjAppendJsonString(&j, systemPrompt);
                LjAppend(&j, "}");
            }
            if (hasMessages) {
                const char* open = strchr(messages, '[');
                const char* close = strrchr(messages, ']');
                if (open && close && close > open) {
                    const char* p = open + 1;
                    size_t n = (size_t) (close - p);
                    while (n && strchr(" \r\n\t", p[n - 1]))
                        n--;
                    while (n && strchr(" \r\n\t", *p)) {
                        p++;
                        n--;
                    }
                    if (n) {
                        if (hasSystem)
                            LjAppend(&j, ",");
                        LjAppend(&j, "%.*s", (int) n, p);
                    }
                }
            }
            LjAppend(&j, "]");
        }
    }
    field(&j, "output", clipped, 1);
    LjAppend(&j,
             ",\"promptTokens\":%d,\"completionTokens\":%d,\"reasoningTokens\":%d,\"errorCode\":%d",
             promptTokens,
             completionTokens,
             reasoningTokens,
             errorCode);
    emit_line("llm", b);
}
