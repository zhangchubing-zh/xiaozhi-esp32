#include "litecrab/hub.h"
#include "litecrab/json.h"
#include "litecrab/kernel.h"
#include "litecrab/observability.h"
#include "litecrab/runtime.h"
#include "litecrab/session.h"
#include "litecrab/tts_sf.h"
#include "litecrab/ws_codec.h"
#include "litecrab/xiaozhi_ws.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define XZ_TAG "[xiaozhi-ws] "
#define XZ_HELLO_TIMEOUT_MS 15000
#define XZ_HTTP_HEAD_TIMEOUT_MS 15000
#define XZ_READ_BUF (32 * 1024)
#define XZ_REPLY_MAX (64 * 1024)
#define XZ_TEXT_MAX 4096
#define XZ_MAX_SENTENCES 128
#define XZ_MCP_TOOLS_MAX 16
#define XZ_MCP_PARAMS_MAX 12
#define XZ_MCP_RESULT_MAX 8192
#define XZ_MCP_CALL_TIMEOUT_MS 20000

typedef struct {
    int fd;
    XiaozhiWsConfig cfg;
} XzClient;

typedef struct XzServerState XzServerState;
typedef struct {
    XzServerState* state;
    int index;
} XzWorkerArg;

struct XzServerState {
    pthread_mutex_t mu;
    pthread_cond_t available;
    XzClient* pending[XIAOZHI_WS_MAX_CONNECTIONS];
    int head, tail, count, total, stopping;
    int workerCount;
    int activeFd[XIAOZHI_WS_MAX_WORKERS];
    pthread_t workers[XIAOZHI_WS_MAX_WORKERS];
    XzWorkerArg workerArgs[XIAOZHI_WS_MAX_WORKERS];
    pthread_t listener;
    int listenerRunning;
    int listenerFd;
};

static XiaozhiWsConfig g_cfg;
static volatile sig_atomic_t g_stopRequested;
static XzServerState g_state;

/* ---- device MCP bridge + thread-safe send ----
 * g_sendMu serializes every frame written to any device socket (worker thread
 * sends tts/stt, agent thread sends tools/call while executing a device tool).
 * g_mcpMu guards the active-session pointer, the pending tools/call slot and
 * the jsonrpc id sequence; g_mcpCv wakes the agent thread when the worker
 * received the matching response (or the session died). */
static pthread_mutex_t g_sendMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_mcpMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mcpCv = PTHREAD_COND_INITIALIZER;

typedef enum {
    XZ_MCP_IDLE = 0,
    XZ_MCP_WAIT_INIT,
    XZ_MCP_WAIT_LIST,
    XZ_MCP_READY
} XzMcpPhase;

/* Storage for dynamically registered device tools. Name/description/enum
 * strings are heap allocated once and intentionally never freed: the runtime
 * registry keeps pointers into this storage for the process lifetime. */
typedef struct {
    char* name;
    char* description;
    CrabToolParamSpec params[XZ_MCP_PARAMS_MAX];
    size_t paramCount;
    char paramNameStorage[XZ_MCP_PARAMS_MAX][CRAB_TOOL_ARG_NAME_MAX];
    char* enumStorage[64];
    const char* enumValues[XZ_MCP_PARAMS_MAX][40];
    int enumUsed;
    int registered;
} XzDeviceTool;

static XzDeviceTool g_deviceTools[XZ_MCP_TOOLS_MAX];
static int g_deviceToolCount;

typedef enum { XZ_STATE_IDLE = 0, XZ_STATE_LISTENING, XZ_STATE_PROCESSING } XzState;

typedef struct XzSession XzSession;
struct XzSession {
    int fd;
    XiaozhiWsConfig* cfg;
    char sessionId[64], userId[64];
    XiaozhiAsrProvider* asr;
    XzState state;
    WsParser parser;
    unsigned char* msgBuf;
    int helloSeen;
    char disconnected;
    int alive;
    XzMcpPhase mcpPhase;
    int mcpBootstrapId;
    char mcpNextCursor[128];
};

/* slot for the single in-flight tools/call (agent thread is serial) */
static struct {
    int id;
    int active;
    int done;
    int failed;
    char result[XZ_MCP_RESULT_MAX];
} g_mcpCall;
static XzSession* g_activeSession;

void XiaozhiWsRequestStop(void) { g_stopRequested = 1; }

int XiaozhiWsValidate(const XiaozhiWsConfig* config, char* error, size_t errorSize) {
    if (error && errorSize)
        error[0] = 0;
    if (!config)
        return -1;
    if (config->listenPort < 1 || config->listenPort > 65535) {
        snprintf(error, errorSize, "xiaozhi_ws: invalid listen port");
        return -1;
    }
    if (config->workerThreads < 1 || config->workerThreads > XIAOZHI_WS_MAX_WORKERS ||
        config->maxConnections < config->workerThreads ||
        config->maxConnections > XIAOZHI_WS_MAX_CONNECTIONS) {
        snprintf(error, errorSize, "xiaozhi_ws: invalid worker/connection limits");
        return -1;
    }
    if (config->maxPayloadBytes < 1024 || config->maxPayloadBytes > 4 * 1024 * 1024) {
        snprintf(error, errorSize, "xiaozhi_ws: invalid max_payload_bytes");
        return -1;
    }
    if (config->agentReplyTimeoutMs < 1000 || config->agentReplyTimeoutMs > 600000 ||
        config->idleTimeoutMs < 10000 || config->idleTimeoutMs > 3600000) {
        snprintf(error, errorSize, "xiaozhi_ws: invalid timeouts");
        return -1;
    }
    if (config->requireAuth && !config->authToken[0]) {
        snprintf(error, errorSize, "xiaozhi_ws: require_auth needs auth_token");
        return -1;
    }
    if (strcmp(config->asr.provider, "siliconflow") && strcmp(config->asr.provider, "stub")) {
        snprintf(error, errorSize, "xiaozhi_ws: unknown asr provider: %s", config->asr.provider);
        return -1;
    }
    if (config->asr.timeoutMs < 1000 || config->asr.timeoutMs > 120000 ||
        config->asr.maxAudioSeconds < 1 || config->asr.maxAudioSeconds > 300) {
        snprintf(error, errorSize, "xiaozhi_ws: invalid asr limits");
        return -1;
    }
    return 0;
}

int XiaozhiWsCheckAuth(const char* authorizationHeader, const char* token) {
    if (!token || !*token)
        return 1;
    if (!authorizationHeader || !*authorizationHeader)
        return 0;
    if (!strcmp(authorizationHeader, token))
        return 1;
    const char* bearer = "Bearer ";
    size_t bl = strlen(bearer);
    if (!strncmp(authorizationHeader, bearer, bl) && !strcmp(authorizationHeader + bl, token))
        return 1;
    return 0;
}

int XiaozhiBuildCheckpointJson(char* out, size_t outSize, const char* url, const char* token) {
    LjBuf b;
    LjBufInit(&b, out, outSize);
    LjAppend(&b, "{\"server_time\":{\"timestamp\":%lld},\"websocket\":{\"url\":", (long long) time(NULL) * 1000);
    LjAppendJsonString(&b, url);
    LjAppend(&b, ",\"token\":");
    LjAppendJsonString(&b, token ? token : "");
    LjAppend(&b, ",\"version\":1}}");
    return b.failed ? -1 : 0;
}

static int utf8_collect(const char* p, int* advance) {
    unsigned char c = (unsigned char) *p;
    *advance = 1;
    if (c == 0xE3 && (unsigned char) p[1] == 0x80 && (unsigned char) p[2] == 0x82) {
        *advance = 3; /* 。 */
        return 1;
    }
    if (c == 0xEF && (unsigned char) p[1] == 0xBC) {
        unsigned char t = (unsigned char) p[2];
        if (t == 0x81 || t == 0x9F || t == 0x9B) {
            *advance = 3; /* ！ ？ ； */
            return 1;
        }
    }
    return 0;
}

int XiaozhiSplitSentences(const char* text, int (*emit)(const char* sentence, void* user), void* user) {
    if (!text || !emit)
        return -1;
    char sentence[512];
    size_t n = 0;
    int emitted = 0;
    const char* p = text;
    while (*p) {
        int advance = 1;
        int boundary = utf8_collect(p, &advance);
        unsigned char c = (unsigned char) *p;
        if (!boundary && c < 0x80 && (c == '!' || c == '?' || c == ';' || c == '.' || c == '\n'))
            boundary = 1;
        if (!boundary) {
            if (c != ' ' || n != 0) {
                if (n >= 120) {
                    /* hard cut without splitting a UTF-8 sequence */
                    while (n > 0 && ((unsigned char) sentence[n - 1] & 0xC0) == 0x80)
                        n--;
                    if (n && (unsigned char) sentence[n - 1] >= 0x80)
                        n--;
                    sentence[n] = 0;
                    if (emit(sentence, user))
                        return emitted;
                    emitted++;
                    n = 0;
                    continue;
                }
                sentence[n++] = *p;
            }
            p += advance;
            continue;
        }
        while (n && sentence[n - 1] == ' ')
            n--;
        sentence[n] = 0;
        if (n) {
            if (emit(sentence, user))
                return emitted;
            emitted++;
            if (emitted >= XZ_MAX_SENTENCES)
                return emitted;
        }
        n = 0;
        p += advance;
    }
    while (n && sentence[n - 1] == ' ')
        n--;
    sentence[n] = 0;
    if (n) {
        emit(sentence, user);
        emitted++;
    }
    return emitted;
}

static int xz_send_all(int fd, const char* d, size_t n) {
    while (n) {
        ssize_t w = send(fd, d, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        d += w;
        n -= (size_t) w;
    }
    return 0;
}

/* Every device-socket write goes through g_sendMu: the worker thread sends
 * protocol frames while the agent thread may concurrently send a tools/call
 * request while executing a device MCP tool. */
static int xz_send_json(int fd, const char* json) {
    size_t n = strlen(json);
    size_t cap = n + 16;
    unsigned char stack[16 + 512];
    unsigned char* frame = cap <= sizeof stack ? stack : malloc(cap);
    if (!frame)
        return -1;
    int total = WsEncodeFrame(WS_MSG_TEXT, (const unsigned char*) json, n, frame, cap);
    pthread_mutex_lock(&g_sendMu);
    int rc = total < 0 ? -1 : xz_send_all(fd, (char*) frame, (size_t) total);
    pthread_mutex_unlock(&g_sendMu);
    if (frame != stack)
        free(frame);
    return rc;
}

static int xz_send_frame_bytes(int fd, int opcode, const unsigned char* payload, size_t len) {
    size_t cap = len + 16;
    unsigned char* frame = malloc(cap);
    if (!frame)
        return -1;
    int total = WsEncodeFrame(opcode, payload, len, frame, cap);
    pthread_mutex_lock(&g_sendMu);
    int rc = total < 0 ? -1 : xz_send_all(fd, (char*) frame, (size_t) total);
    pthread_mutex_unlock(&g_sendMu);
    free(frame);
    return rc;
}

static int xz_send_binary(int fd, const unsigned char* payload, size_t len) {
    return xz_send_frame_bytes(fd, WS_MSG_BINARY, payload, len);
}

static int xz_send_control(int fd, int opcode, const unsigned char* payload, size_t len) {
    return xz_send_frame_bytes(fd, opcode, payload, len);
}

static void xz_derive_session(const char* deviceId, char* sessionId, size_t sidSize, char* userId, size_t uidSize) {
    char mac[24] = "";
    int n = 0;
    const char* src = deviceId ? deviceId : "";
    for (const char* p = src; *p && n < (int) sizeof mac - 1; p++) {
        if (*p == ':')
            continue;
        mac[n++] = (char) tolower((unsigned char) *p);
    }
    mac[n] = 0;
    if (!n)
        snprintf(mac, sizeof mac, "anon%lx", (unsigned long) (time(NULL) & 0xffff));
    snprintf(sessionId, sidSize, "xiaozhi-%s", mac);
    snprintf(userId, uidSize, "xiaozhi:%s", src[0] ? src : "unknown");
}

static void xz_emit_session(LiteMsgType type, const char* userId, const char* sessionId) {
    IngressOptions o = {.source = "gateway:xiaozhi-ws",
                        .userId = userId,
                        .sessionId = sessionId,
                        .type = type,
                        .priority = LITE_PRIORITY_NORMAL,
                        .replyMode = LITE_REPLY_ACK_ONLY};
    IngressResult r;
    IngressSubmit("gateway:xiaozhi-ws",
                  type == LITE_MSG_SESSION_OPEN ? "open" : "close",
                  type == LITE_MSG_SESSION_OPEN ? 4 : 5,
                  &o,
                  &r);
}

static int xz_local_address_ip(int fd, char* out, size_t outSize) {
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    if (getsockname(fd, (struct sockaddr*) &a, &len) || a.sin_family != AF_INET)
        return -1;
    if (!inet_ntop(AF_INET, &a.sin_addr, out, (socklen_t) outSize))
        return -1;
    return 0;
}

static int xz_handle_checkpoint(int fd, WsHandshakeRequest* req, size_t bodyHave) {
    long want = req->contentLength - (long) bodyHave;
    char drain[4096];
    while (want > 0) {
        size_t chunk = want > (long) sizeof drain ? sizeof drain : (size_t) want;
        ssize_t r = recv(fd, drain, chunk, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        want -= r;
    }
    char ip[64];
    if (g_cfg.advertisedIp[0])
        snprintf(ip, sizeof ip, "%s", g_cfg.advertisedIp);
    else if (xz_local_address_ip(fd, ip, sizeof ip))
        snprintf(ip, sizeof ip, "127.0.0.1");
    char url[256], json[1024];
    snprintf(url, sizeof url, "ws://%s:%d/xiaozhi/v1/", ip, g_cfg.listenPort);
    if (XiaozhiBuildCheckpointJson(json, sizeof json, url, g_cfg.authToken)) {
        static const char busy[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        xz_send_all(fd, busy, sizeof busy - 1);
        return -1;
    }
    char head[256];
    int headLen = snprintf(head,
                           sizeof head,
                           "HTTP/1.1 200 OK\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           strlen(json));
    if (headLen <= 0 || xz_send_all(fd, head, (size_t) headLen) || xz_send_all(fd, json, strlen(json)))
        return -1;
    LogPrint(XZ_TAG "checkpoint answered device=%s url=%s", req->deviceId[0] ? req->deviceId : "-", url);
    return 0;
}

typedef struct {
    const char* requestId;
    int timeoutMs;
    char* reply;
    int replySize;
    pthread_mutex_t mu;
    int done;
} XzWaitCtx;

static void* xz_wait_thread(void* arg) {
    XzWaitCtx* w = arg;
    int rc = DispatchResponse(w->requestId, LITE_REPLY_SYNC, w->timeoutMs, w->reply, w->replySize);
    pthread_mutex_lock(&w->mu);
    w->done = rc ? 2 : 1;
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

typedef struct {
    XzSession* session;
    int count;
} XzSentenceCtx;

static int xz_send_stt(XzSession* s, const char* text) {
    char json[XZ_TEXT_MAX];
    LjBuf b;
    LjBufInit(&b, json, sizeof json);
    LjAppend(&b, "{\"type\":\"stt\",\"session_id\":");
    LjAppendJsonString(&b, s->sessionId);
    LjAppend(&b, ",\"text\":");
    LjAppendJsonString(&b, text);
    LjAppend(&b, "}");
    return b.failed ? -1 : xz_send_json(s->fd, json);
}

static int xz_send_tts(XzSession* s, const char* state) {
    char json[256];
    LjBuf b;
    LjBufInit(&b, json, sizeof json);
    LjAppend(&b, "{\"type\":\"tts\",\"session_id\":");
    LjAppendJsonString(&b, s->sessionId);
    LjAppend(&b, ",\"state\":\"%s\"}", state);
    return b.failed ? -1 : xz_send_json(s->fd, json);
}

static int xz_sentence_emit(const char* sentence, void* user) {
    XzSentenceCtx* c = user;
    char json[XZ_TEXT_MAX];
    LjBuf b;
    LjBufInit(&b, json, sizeof json);
    LjAppend(&b, "{\"type\":\"tts\",\"session_id\":");
    LjAppendJsonString(&b, c->session->sessionId);
    LjAppend(&b, ",\"state\":\"sentence_start\",\"text\":");
    LjAppendJsonString(&b, sentence);
    LjAppend(&b, "}");
    if (b.failed)
        return -1;
    if (xz_send_json(c->session->fd, json))
        return -1;
    if (TtsSfEnabled()) {
        unsigned char* frames = NULL;
        size_t framesLen = 0;
        if (!TtsSfSynthesize(sentence, &frames, &framesLen)) {
            size_t off = 0;
            size_t frameCount = 0;
            const int frameMs = TtsSfFrameMs();
            while (off + 2 <= framesLen) {
                size_t frameLen = ((size_t) frames[off] << 8) | frames[off + 1];
                off += 2;
                if (off + frameLen > framesLen)
                    break;
                if (xz_send_binary(c->session->fd, frames + off, frameLen)) {
                    off = framesLen;
                    break;
                }
                off += frameLen;
                frameCount++;
            }
            LogPrint(XZ_TAG "tts sent frames=%zu duration_ms=%zu text=%.60s",
                     frameCount,
                     frameCount * (size_t) frameMs,
                     sentence);
        } else {
            LogPrint(XZ_TAG "tts failed for sentence, audio skipped text=%.60s", sentence);
        }
        free(frames);
    }
    c->count++;
    return 0;
}

static int xz_speak_reply(XzSession* s, const char* reply) {
    if (xz_send_tts(s, "start"))
        return -1;
    XzSentenceCtx ctx = {.session = s, .count = 0};
    XiaozhiSplitSentences(reply, xz_sentence_emit, &ctx);
    if (!ctx.count)
        xz_sentence_emit("（无回复内容）", &ctx);
    return xz_send_tts(s, "stop");
}

static void xz_send_llm_emotion(XzSession* s) {
    char json[256];
    snprintf(json, sizeof json, "{\"type\":\"llm\",\"session_id\":\"%s\",\"emotion\":\"happy\"}", s->sessionId);
    xz_send_json(s->fd, json);
}

/* ---- device MCP bridge ------------------------------------------------- */

static int xz_mcp_next_id(void) {
    static int seq = 100;
    return ++seq;
}

static int xz_mcp_send_request(XzSession* s, const char* method, const char* paramsJson) {
    char json[2048];
    LjBuf b;
    LjBufInit(&b, json, sizeof json);
    LjAppend(&b, "{\"session_id\":");
    LjAppendJsonString(&b, s->sessionId);
    LjAppend(&b, ",\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"method\":");
    LjAppendJsonString(&b, method);
    if (paramsJson && *paramsJson) {
        LjAppend(&b, ",\"params\":%s,\"id\":%d}}", paramsJson, s->mcpBootstrapId);
    } else {
        LjAppend(&b, ",\"id\":%d}", s->mcpBootstrapId);
        LjAppend(&b, "}");
    }
    return b.failed ? -1 : xz_send_json(s->fd, json);
}

/* Extracts result.content[0].text (or error.message) from a JSON-RPC
 * response object into a plain C string. */
static int xz_mcp_extract_payload_text(const char* json, LjToken* tokens, int count,
                                       int root, char* out, size_t outSize) {
    out[0] = 0;
    int result = LjObjectGet(json, tokens, count, root, "result");
    if (result >= 0) {
        int content = LjObjectGet(json, tokens, count, result, "content");
        int item = content >= 0 ? LjArrayGet(tokens, count, content, 0) : -1;
        int text = item >= 0 ? LjObjectGet(json, tokens, count, item, "text") : -1;
        if (text >= 0 && tokens[text].type == LJ_STRING)
            return LjString(json, &tokens[text], out, outSize) ? -1 : 0;
        snprintf(out, outSize, "%s", "(empty result)");
        return 0;
    }
    int error = LjObjectGet(json, tokens, count, root, "error");
    if (error >= 0) {
        int message = LjObjectGet(json, tokens, count, error, "message");
        if (message >= 0 && tokens[message].type == LJ_STRING)
            return LjString(json, &tokens[message], out, outSize) ? -1 : 0;
        snprintf(out, outSize, "%s", "(unknown error)");
        return 0;
    }
    return -1;
}

static char* xz_mcp_store_string(const char* src, size_t len) {
    char* copy = malloc(len + 1);
    if (copy) {
        memcpy(copy, src, len);
        copy[len] = 0;
    }
    return copy;
}

/* Converts one MCP tool description (name/description/inputSchema) into a
 * CrabToolSpec and registers it with the kernel so the LLM can call it. */
static int xz_mcp_tool_execute(CrabRuntime* rt, const CrabToolCall* call, CrabRawResult* raw);
static int xz_mcp_register_tool(const char* json, LjToken* tokens, int count, int toolObj) {
    if (g_deviceToolCount >= XZ_MCP_TOOLS_MAX)
        return -1;
    XzDeviceTool* t = &g_deviceTools[g_deviceToolCount];
    memset(t, 0, sizeof *t);
    int nameTok = LjObjectGet(json, tokens, count, toolObj, "name");
    int descTok = LjObjectGet(json, tokens, count, toolObj, "description");
    if (nameTok < 0 || tokens[nameTok].type != LJ_STRING)
        return -1;
    char name[CRAB_TOOL_NAME_MAX], description[512] = "";
    if (LjString(json, &tokens[nameTok], name, sizeof name))
        return -1;
    if (descTok >= 0 && tokens[descTok].type == LJ_STRING)
        LjString(json, &tokens[descTok], description, sizeof description);
    int schema = LjObjectGet(json, tokens, count, toolObj, "inputSchema");
    int paramsObj = schema >= 0 ? LjObjectGet(json, tokens, count, schema, "properties") : -1;
    int requiredArr = schema >= 0 ? LjObjectGet(json, tokens, count, schema, "required") : -1;
    size_t paramCount = 0;
    if (paramsObj >= 0 && tokens[paramsObj].type == LJ_OBJECT) {
        for (int p = paramsObj + 1; p < count && tokens[p].start < tokens[paramsObj].end;) {
            if (paramCount >= XZ_MCP_PARAMS_MAX)
                break;
            if (tokens[p].type != LJ_STRING)
                break;
            CrabToolParamSpec* spec = &t->params[paramCount];
            memset(spec, 0, sizeof *spec);
            char pname[CRAB_TOOL_ARG_NAME_MAX];
            if (LjString(json, &tokens[p], pname, sizeof pname))
                break;
            snprintf(t->paramNameStorage[paramCount],
                     sizeof t->paramNameStorage[paramCount],
                     "%s",
                     pname);
            spec->name = t->paramNameStorage[paramCount];
            int pval = p + 1;
            int typeTok = LjObjectGet(json, tokens, count, pval, "type");
            char typeText[24] = "string";
            if (typeTok >= 0)
                LjString(json, &tokens[typeTok], typeText, sizeof typeText);
            if (!strcmp(typeText, "integer") || !strcmp(typeText, "number"))
                spec->type = CRAB_TOOL_VALUE_INT;
            else if (!strcmp(typeText, "boolean"))
                spec->type = CRAB_TOOL_VALUE_BOOL;
            else if (!strcmp(typeText, "array"))
                spec->type = CRAB_TOOL_VALUE_STRING_ARRAY;
            else
                spec->type = CRAB_TOOL_VALUE_STRING;
            int minTok = LjObjectGet(json, tokens, count, pval, "minimum");
            if (minTok >= 0)
                LjInt64(json, &tokens[minTok], &spec->minInt);
            int maxTok = LjObjectGet(json, tokens, count, pval, "maximum");
            if (maxTok >= 0)
                LjInt64(json, &tokens[maxTok], &spec->maxInt);
            int enumTok = LjObjectGet(json, tokens, count, pval, "enum");
            if (enumTok >= 0 && tokens[enumTok].type == LJ_ARRAY && t->enumUsed < 40) {
                int enumCount = 0;
                for (int e = enumTok + 1; e < count && tokens[e].start < tokens[enumTok].end; e++) {
                    if (tokens[e].type != LJ_STRING || enumCount >= 40)
                        break;
                    char value[64];
                    if (LjString(json, &tokens[e], value, sizeof value))
                        break;
                    t->enumStorage[t->enumUsed] = xz_mcp_store_string(value, strlen(value));
                    if (!t->enumStorage[t->enumUsed])
                        break;
                    t->enumValues[paramCount][enumCount] = t->enumStorage[t->enumUsed];
                    enumCount++;
                    t->enumUsed++;
                    e = LjSkip(tokens, count, e) - 1;
                }
                if (enumCount) {
                    spec->enumValues = t->enumValues[paramCount];
                    spec->enumCount = (size_t) enumCount;
                }
            }
            if (requiredArr >= 0 && tokens[requiredArr].type == LJ_ARRAY) {
                for (int r = requiredArr + 1; r < count && tokens[r].start < tokens[requiredArr].end; r++) {
                    if (tokens[r].type != LJ_STRING)
                        break;
                    char req[CRAB_TOOL_ARG_NAME_MAX];
                    if (!LjString(json, &tokens[r], req, sizeof req) && !strcmp(req, pname))
                        spec->required = 1;
                }
            }
            paramCount++;
            /* advance past this key and its (possibly nested) value token */
            p = LjSkip(tokens, count, pval);
        }
    }
    t->name = xz_mcp_store_string(name, strlen(name));
    t->description = xz_mcp_store_string(description, strlen(description));
    if (!t->name || !t->description) {
        free(t->name);
        free(t->description);
        for (int i = 0; i < 64; i++)
            free(t->enumStorage[i]);
        return -1;
    }
    static CrabToolSpec toolSpec;
    memset(&toolSpec, 0, sizeof toolSpec);
    toolSpec.name = t->name;
    toolSpec.description = t->description[0] ? t->description : t->name;
    toolSpec.enabledByDefault = 1;
    toolSpec.readonly = 1;
    toolSpec.params = t->params;
    toolSpec.paramCount = paramCount;
    toolSpec.execute = xz_mcp_tool_execute;
    toolSpec.filter = CrabBasicFilter;
    t->paramCount = paramCount;
    if (AgentLoopRegisterTool(&toolSpec)) {
        g_deviceToolCount++; /* claim the slot so storage is not reused */
        return -1;
    }
    t->registered = 1;
    g_deviceToolCount++;
    LogPrint(XZ_TAG "mcp tool registered name=%s params=%zu", t->name, paramCount);
    return 0;
}

/* Handles an incoming {"type":"mcp",...} text message from the device:
 * drives the bootstrap state machine (initialize -> tools/list pages ->
 * register tools) and completes the pending tools/call of the agent thread. */
static void xz_mcp_handle_response(XzSession* s, const char* json, size_t len) {
    if (!json || len >= XZ_TEXT_MAX)
        return;
    char buf[XZ_TEXT_MAX];
    memcpy(buf, json, len);
    buf[len] = 0;
    size_t cap = len / 2 + 64;
    LjToken* tokens = calloc(cap, sizeof *tokens);
    if (!tokens)
        return;
    LjParser parser;
    LjInit(&parser);
    int count = LjParse(&parser, buf, len, tokens, (unsigned) cap);
    if (count < 1) {
        free(tokens);
        return;
    }
    int payload = LjObjectGet(buf, tokens, count, 0, "payload");
    if (payload < 0 || tokens[payload].type != LJ_OBJECT) {
        free(tokens);
        return;
    }
    int idTok = LjObjectGet(buf, tokens, count, payload, "id");
    int64_t id = -1;
    if (idTok < 0 || LjInt64(buf, &tokens[idTok], &id)) {
        free(tokens);
        return;
    }
    if (s->mcpPhase == XZ_MCP_WAIT_INIT && id == s->mcpBootstrapId) {
        int result = LjObjectGet(buf, tokens, count, payload, "result");
        if (result >= 0) {
            s->mcpPhase = XZ_MCP_WAIT_LIST;
            s->mcpBootstrapId = xz_mcp_next_id();
            char params[192];
            snprintf(params, sizeof params, "{\"cursor\":\"%s\",\"withUserTools\":false}",
                     s->mcpNextCursor);
            xz_mcp_send_request(s, "tools/list", params);
            LogPrint(XZ_TAG "mcp initialized, requesting tool list session=%s", s->sessionId);
        } else {
            LogPrint(XZ_TAG "mcp initialize rejected session=%s", s->sessionId);
            s->mcpPhase = XZ_MCP_IDLE;
        }
        free(tokens);
        return;
    }
    if (s->mcpPhase == XZ_MCP_WAIT_LIST && id == s->mcpBootstrapId) {
        int result = LjObjectGet(buf, tokens, count, payload, "result");
        int tools = result >= 0 ? LjObjectGet(buf, tokens, count, result, "tools") : -1;
        if (tools >= 0 && tokens[tools].type == LJ_ARRAY) {
            for (int t = tools + 1; t < count && tokens[t].start < tokens[tools].end; t++) {
                if (tokens[t].type == LJ_OBJECT)
                    xz_mcp_register_tool(buf, tokens, count, t);
                t = LjSkip(tokens, count, t) - 1;
            }
        }
        int cursorTok = result >= 0 ? LjObjectGet(buf, tokens, count, result, "nextCursor") : -1;
        if (cursorTok >= 0 && tokens[cursorTok].type == LJ_STRING &&
            !LjString(buf, &tokens[cursorTok], s->mcpNextCursor, sizeof s->mcpNextCursor) &&
            s->mcpNextCursor[0]) {
            s->mcpBootstrapId = xz_mcp_next_id();
            char params[192];
            snprintf(params, sizeof params, "{\"cursor\":\"%s\",\"withUserTools\":false}",
                     s->mcpNextCursor);
            xz_mcp_send_request(s, "tools/list", params);
            free(tokens);
            return;
        }
        s->mcpPhase = XZ_MCP_READY;
        pthread_mutex_lock(&g_mcpMu);
        g_activeSession = s;
        pthread_mutex_unlock(&g_mcpMu);
        LogPrint(XZ_TAG "mcp ready, device tools=%d session=%s", g_deviceToolCount, s->sessionId);
        free(tokens);
        return;
    }
    /* tools/call response for the agent thread */
    pthread_mutex_lock(&g_mcpMu);
    if (g_mcpCall.active && id == g_mcpCall.id) {
        if (xz_mcp_extract_payload_text(buf, tokens, count, payload,
                                        g_mcpCall.result, sizeof g_mcpCall.result))
            snprintf(g_mcpCall.result, sizeof g_mcpCall.result, "(malformed response)");
        g_mcpCall.done = 1;
        pthread_cond_broadcast(&g_mcpCv);
    }
    pthread_mutex_unlock(&g_mcpMu);
    free(tokens);
}

static void xz_mcp_bootstrap(XzSession* s) {
    if (s->cfg->mcpEnabled == 0)
        return;
    s->mcpPhase = XZ_MCP_WAIT_INIT;
    s->mcpNextCursor[0] = 0;
    s->mcpBootstrapId = xz_mcp_next_id();
    if (xz_mcp_send_request(s, "initialize", "{\"capabilities\":{}}"))
        s->mcpPhase = XZ_MCP_IDLE;
}

static void xz_mcp_session_closed(XzSession* s) {
    s->alive = 0;
    s->mcpPhase = XZ_MCP_IDLE;
    pthread_mutex_lock(&g_mcpMu);
    if (g_activeSession == s)
        g_activeSession = NULL;
    if (g_mcpCall.active && !g_mcpCall.done) {
        g_mcpCall.failed = 1;
        g_mcpCall.done = 1;
        pthread_cond_broadcast(&g_mcpCv);
    }
    pthread_mutex_unlock(&g_mcpMu);
}

/* CrabToolSpec.execute callback: runs on the agent thread while the LLM asked
 * for a device tool. Serializes the typed arguments back into JSON, sends a
 * tools/call to the active device and waits for the worker thread to receive
 * the matching JSON-RPC response. */
static int xz_mcp_tool_execute(CrabRuntime* rt, const CrabToolCall* call, CrabRawResult* raw) {
    (void) rt;
    if (!call || !raw)
        return CRAB_ERROR_INVALID_ARG;
    char arguments[2048];
    {
        LjBuf b;
        LjBufInit(&b, arguments, sizeof arguments);
        LjAppend(&b, "{");
        for (size_t i = 0; i < call->argCount; i++) {
            if (i)
                LjAppend(&b, ",");
            const CrabToolArg* arg = &call->args[i];
            LjAppend(&b, "\"%s\":", arg->name);
            switch (arg->value.type) {
            case CRAB_TOOL_VALUE_INT:
                LjAppend(&b, "%lld", (long long) arg->value.intValue);
                break;
            case CRAB_TOOL_VALUE_BOOL:
                LjAppend(&b, "%s", arg->value.boolValue ? "true" : "false");
                break;
            case CRAB_TOOL_VALUE_STRING_ARRAY: {
                LjAppend(&b, "[");
                for (size_t j = 0; j < arg->value.stringArrayValue.count; j++) {
                    if (j)
                        LjAppend(&b, ",");
                    LjAppendJsonString(&b, arg->value.stringArrayValue.items[j]);
                }
                LjAppend(&b, "]");
                break;
            }
            default:
                LjAppendJsonString(&b, arg->value.stringValue ? arg->value.stringValue : "");
                break;
            }
        }
        LjAppend(&b, "}");
        if (b.failed)
            return CRAB_ERROR_EXECUTE_FAILED;
    }
    pthread_mutex_lock(&g_mcpMu);
    XzSession* s = g_activeSession;
    if (!s || !s->alive || s->mcpPhase != XZ_MCP_READY) {
        pthread_mutex_unlock(&g_mcpMu);
        CrabRawResultSetStdout(raw, "ERROR: no connected xiaozhi device");
        return 0;
    }
    int callId = xz_mcp_next_id();
    g_mcpCall.id = callId;
    g_mcpCall.active = 1;
    g_mcpCall.done = 0;
    g_mcpCall.failed = 0;
    g_mcpCall.result[0] = 0;
    pthread_mutex_unlock(&g_mcpMu);
    char json[3072];
    LjBuf b;
    LjBufInit(&b, json, sizeof json);
    LjAppend(&b, "{\"session_id\":");
    LjAppendJsonString(&b, s->sessionId);
    LjAppend(&b, ",\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\","
                 "\"params\":{\"name\":");
    LjAppendJsonString(&b, call->toolName);
    LjAppend(&b, ",\"arguments\":%s},\"id\":%d}}", arguments, callId);
    if (b.failed || xz_send_json(s->fd, json)) {
        pthread_mutex_lock(&g_mcpMu);
        g_mcpCall.active = 0;
        pthread_mutex_unlock(&g_mcpMu);
        CrabRawResultSetStdout(raw, "ERROR: device send failed");
        return 0;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += XZ_MCP_CALL_TIMEOUT_MS / 1000;
    deadline.tv_nsec += (XZ_MCP_CALL_TIMEOUT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_mcpMu);
    while (g_mcpCall.active && !g_mcpCall.done) {
        if (pthread_cond_timedwait(&g_mcpCv, &g_mcpMu, &deadline) == ETIMEDOUT)
            break;
    }
    int failed = g_mcpCall.failed || !g_mcpCall.done;
    char result[XZ_MCP_RESULT_MAX];
    snprintf(result, sizeof result, "%s",
             g_mcpCall.done && g_mcpCall.result[0] ? g_mcpCall.result
                                                   : "ERROR: device tool call timeout");
    g_mcpCall.active = 0;
    pthread_mutex_unlock(&g_mcpMu);
    if (failed)
        LogPrint(XZ_TAG "mcp tool call failed tool=%s", call->toolName);
    CrabRawResultSetStdout(raw, result);
    return 0;
}

static void xz_handle_text(XzSession* s, const unsigned char* data, size_t len);
static int xz_agent_turn(XzSession* s, const char* text) {
    if (!text || !*text)
        return 0;
    IngressOptions o = {.source = "gateway:xiaozhi-ws",
                        .userId = s->userId,
                        .sessionId = s->sessionId,
                        .type = LITE_MSG_CHAT,
                        .priority = LITE_PRIORITY_NORMAL,
                        .replyMode = LITE_REPLY_SYNC};
    o.deadlineMs = LiteMonotonicMs() + s->cfg->agentReplyTimeoutMs;
    IngressResult r;
    if (IngressSubmit("gateway:xiaozhi-ws", text, (int) strlen(text), &o, &r)) {
        xz_speak_reply(s, "ERROR: 请求被拒绝，请稍后再试。");
        return 0;
    }
    char* reply = malloc(XZ_REPLY_MAX);
    if (!reply) {
        RequestCancel(r.requestId);
        return -1;
    }
    XzWaitCtx wait = {.requestId = r.requestId,
                      .timeoutMs = s->cfg->agentReplyTimeoutMs,
                      .reply = reply,
                      .replySize = XZ_REPLY_MAX};
    pthread_mutex_init(&wait.mu, NULL);
    pthread_t waiter;
    if (pthread_create(&waiter, NULL, xz_wait_thread, &wait)) {
        RequestCancel(r.requestId);
        pthread_mutex_destroy(&wait.mu);
        free(reply);
        return -1;
    }
    int64_t deadline = LiteMonotonicMs() + s->cfg->agentReplyTimeoutMs;
    int cancelled = 0;
    unsigned char buf[4096];
    char pending[4][XZ_TEXT_MAX];
    size_t pendingLen[4];
    int pendingCount = 0;
    for (;;) {
        pthread_mutex_lock(&wait.mu);
        int done = wait.done;
        pthread_mutex_unlock(&wait.mu);
        if (done)
            break;
        struct pollfd pfd = {.fd = s->fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 50);
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr < 0)
            break;
        if (!pr) {
            if (LiteMonotonicMs() >= deadline && !cancelled) {
                RequestCancel(r.requestId);
                cancelled = 2;
            }
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (!cancelled) {
                RequestCancel(r.requestId);
                cancelled = 3;
            }
            continue;
        }
        if (!(pfd.revents & POLLIN))
            continue;
        ssize_t n = recv(s->fd, buf, sizeof buf, 0);
        if (n == 0) {
            if (!cancelled) {
                RequestCancel(r.requestId);
                cancelled = 3;
            }
            s->disconnected = 1;
            break;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (!cancelled) {
                RequestCancel(r.requestId);
                cancelled = 3;
            }
            s->disconnected = 1;
            break;
        }
        size_t off = 0;
        while (off < (size_t) n && !cancelled && !s->disconnected) {
            size_t used = 0;
            WsMessage msg;
            int fr = WsParserFeed(&s->parser, buf + off, (size_t) n - off, &used, &msg);
            if (fr < 0)
                break;
            off += used;
            if (fr != 1)
                break;
            if (msg.type == WS_MSG_PING) {
                xz_send_control(s->fd, WS_MSG_PONG, msg.data, msg.len);
            } else if (msg.type == WS_MSG_CLOSE) {
                xz_send_control(s->fd, WS_MSG_CLOSE, msg.data, msg.len);
                RequestCancel(r.requestId);
                cancelled = 3;
                s->disconnected = 1;
            } else if (msg.type == WS_MSG_TEXT) {
                char json[XZ_TEXT_MAX];
                size_t jlen = msg.len < XZ_TEXT_MAX - 1 ? msg.len : XZ_TEXT_MAX - 1;
                memcpy(json, msg.data, jlen);
                json[jlen] = 0;
                char type[32] = "";
                JsonExtractStringField(json, "type", type, sizeof type);
                if (!strcmp(type, "abort")) {
                    RequestCancel(r.requestId);
                    cancelled = 1;
                    LogPrint(XZ_TAG "abort while processing session=%s", s->sessionId);
                } else if (!strcmp(type, "mcp")) {
                    if (s->mcpPhase == XZ_MCP_READY) {
                        /* tools/call response for the agent thread: completing
                         * it never touches the kernel tools mutex. */
                        xz_mcp_handle_response(s, json, jlen);
                    } else if (pendingCount < 4 && jlen < XZ_TEXT_MAX) {
                        /* Bootstrap responses register device tools, which can
                         * block on the agent's tools mutex for the whole LLM
                         * call; deferring keeps abort handling responsive. */
                        memcpy(pending[pendingCount], json, jlen + 1);
                        pendingLen[pendingCount] = jlen;
                        pendingCount++;
                    }
                } else if (pendingCount < 4 && jlen < XZ_TEXT_MAX) {
                    /* A new turn arriving mid-processing is replayed after
                     * this turn finishes; the protocol expects the device to
                     * wait, but queueing keeps the gateway robust. */
                    memcpy(pending[pendingCount], json, jlen + 1);
                    pendingLen[pendingCount] = jlen;
                    pendingCount++;
                }
            }
            WsParserConsume(&s->parser);
        }
        if (cancelled || s->disconnected)
            break;
    }
    pthread_join(waiter, NULL);
    pthread_mutex_destroy(&wait.mu);
    int rc = 0;
    if (cancelled == 3 || s->disconnected) {
        rc = -1;
    } else if (cancelled == 2) {
        xz_speak_reply(s, "抱歉，处理超时，请稍后再试。");
    } else if (cancelled == 1) {
        LogPrint(XZ_TAG "turn aborted session=%s", s->sessionId);
    } else if (wait.done == 2) {
        xz_speak_reply(s, "抱歉，agent 处理失败，请稍后再试。");
    } else if (!reply[0]) {
        xz_speak_reply(s, "抱歉，agent 未返回内容，请稍后再试。");
    } else {
        xz_speak_reply(s, reply);
    }
    if (rc == 0 && pendingCount && !s->disconnected) {
        s->state = XZ_STATE_IDLE;
        for (int i = 0; i < pendingCount && !s->disconnected; i++)
            xz_handle_text(s, (const unsigned char*) pending[i], pendingLen[i]);
    }
    free(reply);
    return rc;
}

static int xz_voice_turn(XzSession* s) {
    char text[XZ_TEXT_MAX];
    s->state = XZ_STATE_PROCESSING;
    int rc = s->asr->finish(s->asr, s->sessionId, text, sizeof text);
    if (rc < 0) {
        xz_speak_reply(s, "抱歉，语音识别失败，请稍后再试。");
        s->state = XZ_STATE_IDLE;
        return 0;
    }
    if (rc == 0 && !text[0]) {
        LogPrint(XZ_TAG "empty transcription, skip turn session=%s", s->sessionId);
        s->state = XZ_STATE_IDLE;
        return 0;
    }
    if (rc == 1) {
        char sttText[XZ_TEXT_MAX + 128];
        snprintf(sttText, sizeof sttText, "（语音识别失败，使用预设指令）%s", text);
        xz_send_stt(s, sttText);
    } else {
        xz_send_stt(s, text);
    }
    xz_send_llm_emotion(s);
    LogPrint(XZ_TAG "turn start session=%s source=asr fallback=%d text=%.120s", s->sessionId, rc == 1, text);
    xz_agent_turn(s, text);
    LogPrint(XZ_TAG "turn end session=%s", s->sessionId);
    s->state = XZ_STATE_IDLE;
    return 0;
}

static int xz_direct_turn(XzSession* s, const char* text) {
    s->state = XZ_STATE_PROCESSING;
    xz_send_stt(s, text);
    xz_send_llm_emotion(s);
    LogPrint(XZ_TAG "turn start session=%s source=text text=%.120s", s->sessionId, text);
    xz_agent_turn(s, text);
    LogPrint(XZ_TAG "turn end session=%s", s->sessionId);
    s->state = XZ_STATE_IDLE;
    return 0;
}

static void xz_handle_text(XzSession* s, const unsigned char* data, size_t len) {
    if (len >= XZ_TEXT_MAX)
        len = XZ_TEXT_MAX - 1;
    char json[XZ_TEXT_MAX];
    memcpy(json, data, len);
    json[len] = 0;
    char type[32] = "";
    JsonExtractStringField(json, "type", type, sizeof type);
    if (!strcmp(type, "hello")) {
        s->helloSeen = 1;
        return;
    }
    if (!strcmp(type, "listen")) {
        char state[32] = "";
        JsonExtractStringField(json, "state", state, sizeof state);
        if (!strcmp(state, "start") || !strcmp(state, "detect")) {
            if (s->state != XZ_STATE_LISTENING) {
                s->asr->begin(s->asr, s->sessionId);
                s->state = XZ_STATE_LISTENING;
            }
            return;
        }
        if (!strcmp(state, "stop")) {
            if (s->state == XZ_STATE_LISTENING)
                xz_voice_turn(s);
            return;
        }
        return;
    }
    if (!strcmp(type, "abort")) {
        LogPrint(XZ_TAG "abort in state=%d session=%s", s->state, s->sessionId);
        return;
    }
    if (!strcmp(type, "chat")) {
        if (s->state == XZ_STATE_IDLE) {
            char text[XZ_TEXT_MAX];
            if (JsonExtractStringField(json, "text", text, sizeof text) && text[0])
                xz_direct_turn(s, text);
        }
        return;
    }
    if (!strcmp(type, "mcp")) {
        xz_mcp_handle_response(s, json, len);
        return;
    }
    LogPrint(XZ_TAG "unhandled message type=%s session=%s", type[0] ? type : "(missing)", s->sessionId);
}

static void xz_handle_message(XzSession* s, WsMessage* msg) {
    if (msg->type == WS_MSG_PING) {
        xz_send_control(s->fd, WS_MSG_PONG, msg->data, msg->len);
        return;
    }
    if (msg->type == WS_MSG_PONG || msg->type == WS_MSG_CLOSE)
        return;
    if (msg->type == WS_MSG_BINARY) {
        if (s->state == XZ_STATE_LISTENING)
            s->asr->feed_audio(s->asr, msg->data, msg->len);
        return;
    }
    if (msg->type == WS_MSG_TEXT)
        xz_handle_text(s, msg->data, msg->len);
}

/* Processes every complete message currently in buf; returns 0 ok, 1 close
 * frame seen, -1 protocol error. Leftover partial bytes move to buf start. */
static int xz_pump(XzSession* s, unsigned char* buf, size_t* buffered) {
    size_t off = 0;
    while (off < *buffered) {
        size_t used = 0;
        WsMessage msg;
        int fr = WsParserFeed(&s->parser, buf + off, *buffered - off, &used, &msg);
        if (fr < 0)
            return -1;
        off += used;
        if (fr != 1)
            break;
        if (msg.type == WS_MSG_CLOSE) {
            xz_send_control(s->fd, WS_MSG_CLOSE, msg.data, msg.len);
            return 1;
        }
        /* Consume BEFORE handling: handlers may run a full agent turn that
         * reads further frames through the same parser, which must not be
         * paused in the READY state. Message data stays valid until the next
         * WsParserFeed call. */
        WsParserConsume(&s->parser);
        xz_handle_message(s, &msg);
        if (s->disconnected)
            return 1;
    }
    if (off) {
        memmove(buf, buf + off, *buffered - off);
        *buffered -= off;
    }
    return 0;
}

static ssize_t xz_recv(int fd, unsigned char* buf, size_t size) {
    for (;;) {
        ssize_t n = recv(fd, buf, size, 0);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

static int xz_session_run(XzClient* c) {
    int fd = c->fd;
    struct timeval tv = {XZ_HTTP_HEAD_TIMEOUT_MS / 1000, (XZ_HTTP_HEAD_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    unsigned char* buf = malloc(XZ_READ_BUF);
    unsigned char* msgBuf = malloc((size_t) c->cfg.maxPayloadBytes);
    if (!buf || !msgBuf)
        goto fail;
    size_t buffered = 0;
    while (buffered < WS_HTTP_HEAD_MAX) {
        ssize_t n = xz_recv(fd, buf + buffered, 4096);
        if (n <= 0)
            goto fail;
        buffered += (size_t) n;
        if (memmem(buf, buffered, "\r\n\r\n", 4))
            break;
    }
    WsHandshakeRequest req;
    if (WsParseHttpHead((const char*) buf, buffered, &req) != 1)
        goto fail;
    if (!req.isUpgrade) {
        xz_handle_checkpoint(fd, &req, buffered - (size_t) req.headerLength);
        goto done;
    }
    if (c->cfg.requireAuth && !XiaozhiWsCheckAuth(req.authorization, c->cfg.authToken)) {
        static const char denied[] = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        xz_send_all(fd, denied, sizeof denied - 1);
        LogPrint(XZ_TAG "handshake auth rejected device=%s", req.deviceId);
        goto fail;
    }
    {
        char accept[32];
        WsComputeAccept(req.wsKey, accept);
        char response[256];
        int responseLen = snprintf(response,
                                   sizeof response,
                                   "HTTP/1.1 101 Switching Protocols\r\n"
                                   "Upgrade: websocket\r\n"
                                   "Connection: Upgrade\r\n"
                                   "Sec-WebSocket-Accept: %s\r\n"
                                   "\r\n",
                                   accept);
        if (responseLen <= 0 || xz_send_all(fd, response, (size_t) responseLen))
            goto fail;
    }
    {
        XzSession s;
        memset(&s, 0, sizeof s);
        s.fd = fd;
        s.cfg = &c->cfg;
        s.msgBuf = msgBuf;
        xz_derive_session(req.deviceId, s.sessionId, sizeof s.sessionId, s.userId, sizeof s.userId);
        if (AgentSessionIdValidate(s.sessionId))
            xz_derive_session("", s.sessionId, sizeof s.sessionId, s.userId, sizeof s.userId);
        WsParserInit(&s.parser, msgBuf, (size_t) c->cfg.maxPayloadBytes);
        s.asr = AsrSfCreateProvider();
        if (!s.asr)
            goto fail;
        size_t leftover = buffered - (size_t) req.headerLength;
        if (leftover)
            memmove(buf, buf + req.headerLength, leftover);
        buffered = leftover;
        while (!s.helloSeen) {
            int pr = xz_pump(&s, buf, &buffered);
            if (pr)
                goto session_fail;
            if (s.helloSeen)
                break;
            if (buffered + 4096 > XZ_READ_BUF)
                goto session_fail;
            ssize_t n = xz_recv(fd, buf + buffered, 4096);
            if (n <= 0)
                goto session_fail;
            buffered += (size_t) n;
        }
        char hello[XZ_TEXT_MAX];
        LjBuf b;
        LjBufInit(&b, hello, sizeof hello);
        LjAppend(&b, "{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":");
        LjAppendJsonString(&b, s.sessionId);
        LjAppend(&b,
                 ",\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,\"channels\":1,"
                 "\"frame_duration\":%d}}",
                 TtsSfEnabled() ? TtsSfSampleRate() : 16000,
                 TtsSfEnabled() ? TtsSfFrameMs() : 60);
        if (b.failed || xz_send_json(fd, hello))
            goto session_fail;
        s.alive = 1;
        xz_mcp_bootstrap(&s);
        xz_emit_session(LITE_MSG_SESSION_OPEN, s.userId, s.sessionId);
        LogPrint(XZ_TAG "session open device=%s client=%s session=%s auth=%d asr=%s opus=%d tts=%d mcp=%d",
                 req.deviceId,
                 req.clientId,
                 s.sessionId,
                 c->cfg.requireAuth,
                 c->cfg.asr.provider,
                 AsrSfHaveOpusDecode(),
                 TtsSfEnabled(),
                 c->cfg.mcpEnabled);
        struct timeval idle = {c->cfg.idleTimeoutMs / 1000, (c->cfg.idleTimeoutMs % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof idle);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &idle, sizeof idle);
        for (;;) {
            int pr = xz_pump(&s, buf, &buffered);
            if (pr)
                break;
            if (buffered + 4096 > XZ_READ_BUF) {
                LogPrint(XZ_TAG "read buffer stuck session=%s", s.sessionId);
                break;
            }
            ssize_t n = xz_recv(fd, buf + buffered, 4096);
            if (n <= 0)
                break;
            buffered += (size_t) n;
        }
        xz_emit_session(LITE_MSG_SESSION_CLOSE, s.userId, s.sessionId);
        LogPrint(XZ_TAG "session close session=%s", s.sessionId);
    session_fail:
        xz_mcp_session_closed(&s);
        if (s.asr) {
            s.asr->abort(s.asr);
            AsrSfDestroyProvider(s.asr);
        }
        goto done;
    }
done:
    free(buf);
    free(msgBuf);
    return 0;
fail:
    free(buf);
    free(msgBuf);
    return -1;
}

static void* xz_connection_worker(void* arg) {
    XzWorkerArg* worker = arg;
    XzServerState* state = worker->state;
    for (;;) {
        pthread_mutex_lock(&state->mu);
        while (!state->count && !state->stopping)
            pthread_cond_wait(&state->available, &state->mu);
        if (state->stopping && !state->count) {
            pthread_mutex_unlock(&state->mu);
            break;
        }
        XzClient* client = state->pending[state->head];
        state->pending[state->head] = NULL;
        state->head = (state->head + 1) % XIAOZHI_WS_MAX_CONNECTIONS;
        state->count--;
        state->activeFd[worker->index] = client->fd;
        pthread_mutex_unlock(&state->mu);
        xz_session_run(client);
        pthread_mutex_lock(&state->mu);
        state->activeFd[worker->index] = -1;
        if (state->total > 0)
            state->total--;
        pthread_mutex_unlock(&state->mu);
        close(client->fd);
        free(client);
    }
    return NULL;
}

static void* xz_listener_main(void* arg) {
    XzServerState* state = arg;
    (void) state;
    while (!g_stopRequested) {
        struct pollfd ready = {.fd = g_state.listenerFd, .events = POLLIN};
        int waitResult = poll(&ready, 1, 100);
        if (waitResult < 0 && errno == EINTR)
            continue;
        if (waitResult < 0)
            break;
        if (!waitResult)
            continue;
        int conn = accept(g_state.listenerFd, NULL, NULL);
        if (conn < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        XzClient* c = malloc(sizeof *c);
        if (!c) {
            close(conn);
            continue;
        }
        *c = (XzClient){conn, g_cfg};
        pthread_mutex_lock(&g_state.mu);
        if (g_state.total >= g_cfg.maxConnections || g_state.count >= XIAOZHI_WS_MAX_CONNECTIONS) {
            pthread_mutex_unlock(&g_state.mu);
            close(conn);
            free(c);
            continue;
        }
        g_state.pending[g_state.tail] = c;
        g_state.tail = (g_state.tail + 1) % XIAOZHI_WS_MAX_CONNECTIONS;
        g_state.count++;
        g_state.total++;
        pthread_cond_signal(&g_state.available);
        pthread_mutex_unlock(&g_state.mu);
    }
    close(g_state.listenerFd);
    g_state.listenerFd = -1;
    RequestCancelAll();
    pthread_mutex_lock(&g_state.mu);
    g_state.stopping = 1;
    for (int i = 0; i < g_state.workerCount; i++)
        if (g_state.activeFd[i] >= 0)
            shutdown(g_state.activeFd[i], SHUT_RDWR);
    while (g_state.count) {
        XzClient* client = g_state.pending[g_state.head];
        g_state.pending[g_state.head] = NULL;
        g_state.head = (g_state.head + 1) % XIAOZHI_WS_MAX_CONNECTIONS;
        g_state.count--;
        if (g_state.total > 0)
            g_state.total--;
        close(client->fd);
        free(client);
    }
    pthread_cond_broadcast(&g_state.available);
    pthread_mutex_unlock(&g_state.mu);
    for (int i = 0; i < g_state.workerCount; i++)
        pthread_join(g_state.workers[i], NULL);
    pthread_cond_destroy(&g_state.available);
    pthread_mutex_destroy(&g_state.mu);
    g_state.listenerRunning = 0;
    return NULL;
}

int XiaozhiWsStart(const XiaozhiWsConfig* in) {
    if (g_state.listenerRunning)
        return -1;
    XiaozhiWsConfig cfg = {0};
    if (in)
        cfg = *in;
    if (!cfg.listenIp[0])
        snprintf(cfg.listenIp, sizeof cfg.listenIp, "0.0.0.0");
    if (cfg.listenPort <= 0)
        cfg.listenPort = 8000;
    if (cfg.agentReplyTimeoutMs <= 0)
        cfg.agentReplyTimeoutMs = 120000;
    if (cfg.maxPayloadBytes <= 0)
        cfg.maxPayloadBytes = 262144;
    if (cfg.workerThreads <= 0)
        cfg.workerThreads = 8;
    if (cfg.maxConnections <= 0)
        cfg.maxConnections = 64;
    if (cfg.idleTimeoutMs <= 0)
        cfg.idleTimeoutMs = 300000;
    if (!cfg.asr.provider[0])
        snprintf(cfg.asr.provider, sizeof cfg.asr.provider, "siliconflow");
    if (cfg.asr.timeoutMs <= 0)
        cfg.asr.timeoutMs = 60000;
    if (cfg.asr.maxAudioSeconds <= 0)
        cfg.asr.maxAudioSeconds = 60;
    char error[256];
    if (XiaozhiWsValidate(&cfg, error, sizeof error)) {
        LogPrint(XZ_TAG "invalid configuration: %s", error);
        return -1;
    }
    g_cfg = cfg;
    AsrSfInit(&g_cfg.asr);
    if (g_cfg.tts.enabled && !TtsSfHaveOpusEncode())
        LogPrint(XZ_TAG "WARNING libopus not compiled in; tts disabled");
    TtsSfInit(&g_cfg.tts);
    if (g_cfg.mcpEnabled)
        LogPrint(XZ_TAG "device MCP bridge enabled (tools registered on connect)");
    if (!strcmp(g_cfg.asr.provider, "siliconflow") && !AsrSfHaveOpusDecode())
        LogPrint(XZ_TAG "WARNING libopus not compiled in; siliconflow ASR will fall back to stub text");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t) g_cfg.listenPort)};
    if (inet_pton(AF_INET, g_cfg.listenIp, &a.sin_addr) != 1 || bind(fd, (struct sockaddr*) &a, sizeof a) ||
        listen(fd, 32)) {
        close(fd);
        return -1;
    }
    memset(&g_state, 0, sizeof g_state);
    g_state.listenerFd = fd;
    pthread_mutex_init(&g_state.mu, NULL);
    pthread_cond_init(&g_state.available, NULL);
    g_state.workerCount = g_cfg.workerThreads;
    for (int i = 0; i < XIAOZHI_WS_MAX_WORKERS; i++)
        g_state.activeFd[i] = -1;
    int started = 0;
    for (; started < g_state.workerCount; started++) {
        g_state.workerArgs[started] = (XzWorkerArg){.state = &g_state, .index = started};
        if (pthread_create(&g_state.workers[started], NULL, xz_connection_worker, &g_state.workerArgs[started]))
            break;
    }
    if (started != g_state.workerCount) {
        pthread_mutex_lock(&g_state.mu);
        g_state.stopping = 1;
        pthread_cond_broadcast(&g_state.available);
        pthread_mutex_unlock(&g_state.mu);
        for (int i = 0; i < started; i++)
            pthread_join(g_state.workers[i], NULL);
        pthread_cond_destroy(&g_state.available);
        pthread_mutex_destroy(&g_state.mu);
        close(fd);
        return -1;
    }
    g_stopRequested = 0;
    if (pthread_create(&g_state.listener, NULL, xz_listener_main, NULL)) {
        g_state.stopping = 1;
        pthread_cond_broadcast(&g_state.available);
        for (int i = 0; i < g_state.workerCount; i++)
            pthread_join(g_state.workers[i], NULL);
        pthread_cond_destroy(&g_state.available);
        pthread_mutex_destroy(&g_state.mu);
        close(fd);
        return -1;
    }
    g_state.listenerRunning = 1;
    LogPrint(XZ_TAG "listening on %s:%d workers=%d max_connections=%d auth=%d asr=%s",
             g_cfg.listenIp,
             g_cfg.listenPort,
             g_cfg.workerThreads,
             g_cfg.maxConnections,
             g_cfg.requireAuth,
             g_cfg.asr.provider);
    return 0;
}

int XiaozhiWsStop(void) {
    if (!g_state.listenerRunning)
        return 0;
    g_stopRequested = 1;
    pthread_join(g_state.listener, NULL);
    return 0;
}
