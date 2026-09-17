#include "litecrab/config.h"

#include "litecrab/json.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* json;
    LjToken* tokens;
    int count;
} ParsedFile;

static void copy_text(char* out, size_t size, const char* value) {
    if (!out || !size)
        return;
    size_t n = value ? strlen(value) : 0;
    if (n >= size)
        n = size - 1;
    if (n)
        memcpy(out, value, n);
    out[n] = 0;
}
static void set_error(char* out, size_t size, const char* fmt, const char* value) {
    if (out && size)
        snprintf(out, size, fmt, value ? value : "");
}
static void parsed_clear(ParsedFile* p) {
    if (p) {
        free(p->json);
        free(p->tokens);
        memset(p, 0, sizeof *p);
    }
}
static int parse_file(const char* path, ParsedFile* out, char* error, size_t errorSize) {
    memset(out, 0, sizeof *out);
    if (!path || !*path)
        return 0;
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        set_error(error, errorSize, "cannot open config: %s", path);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END)) {
        fclose(fp);
        return -1;
    }
    long length = ftell(fp);
    rewind(fp);
    if (length < 2 || length > 1024 * 1024) {
        fclose(fp);
        set_error(error, errorSize, "invalid config size: %s", path);
        return -1;
    }
    out->json = malloc((size_t) length + 1);
    if (!out->json) {
        fclose(fp);
        return -1;
    }
    size_t got = fread(out->json, 1, (size_t) length, fp);
    fclose(fp);
    out->json[got] = 0;
    if (LjValidate(out->json, LJ_OBJECT)) {
        set_error(error, errorSize, "invalid JSON config: %s", path);
        parsed_clear(out);
        return -1;
    }
    size_t cap = got / 2 + 64;
    out->tokens = calloc(cap, sizeof *out->tokens);
    if (!out->tokens) {
        parsed_clear(out);
        return -1;
    }
    LjParser parser;
    LjInit(&parser);
    out->count = LjParse(&parser, out->json, got, out->tokens, (unsigned) cap);
    if (out->count < 1) {
        set_error(error, errorSize, "cannot parse config: %s", path);
        parsed_clear(out);
        return -1;
    }
    return 0;
}
static int object_get(const ParsedFile* p, int object, const char* key) {
    return LjObjectGet(p->json, p->tokens, p->count, object, key);
}
static int get_string(const ParsedFile* p, int object, const char* key, char* out, size_t size) {
    int v = object_get(p, object, key);
    return v >= 0 && p->tokens[v].type == LJ_STRING && !LjString(p->json, &p->tokens[v], out, size);
}
static int get_int(const ParsedFile* p, int object, const char* key, int* out) {
    int v = object_get(p, object, key);
    int64_t n;
    if (v < 0 || LjInt64(p->json, &p->tokens[v], &n) || n < INT32_MIN || n > INT32_MAX)
        return 0;
    *out = (int) n;
    return 1;
}
static int get_double(const ParsedFile* p, int object, const char* key, double* out) {
    int v = object_get(p, object, key);
    return v >= 0 && !LjDouble(p->json, &p->tokens[v], out);
}
static int get_bool(const ParsedFile* p, int object, const char* key, int* out) {
    int v = object_get(p, object, key);
    return v >= 0 && !LjBool(p->json, &p->tokens[v], out);
}

void LiteCrabConfigDefaults(LiteCrabAppConfig* c) {
    if (!c)
        return;
    memset(c, 0, sizeof *c);
    copy_text(c->server.listenIp, sizeof c->server.listenIp, "127.0.0.1");
    c->server.listenPort = 10003;
    c->server.backlog = 32;
    c->server.recvTimeoutMs = 30000;
    c->server.sendTimeoutMs = 30000;
    c->server.maxReqBytes = 16384;
    c->server.workerThreads = 4;
    c->server.maxConnections = 16;
    c->server.allowUnauthenticatedRemote = 0;
    copy_text(c->workspaceRoot, sizeof c->workspaceRoot, ".");
    copy_text(c->logDir, sizeof c->logDir, "logs");
    copy_text(c->llmBaseUrl, sizeof c->llmBaseUrl, "http://127.0.0.1:18080/v1/chat/completions");
    copy_text(c->llmModel, sizeof c->llmModel, "litecrab");
    c->llm.maxTokens = 2048;
    c->llm.temperature = 0.2;
    c->llm.stream = 1;
    c->llm.timeoutMs = 60000;
    c->llm.baseUrl = c->llmBaseUrl;
    c->llm.apiKey = c->llmApiKey;
    c->llm.model = c->llmModel;
    c->llm.reasoningEffort = c->reasoningEffort;
    copy_text(c->xiaozhiWs.listenIp, sizeof c->xiaozhiWs.listenIp, "0.0.0.0");
    c->xiaozhiWs.listenPort = 8000;
    c->xiaozhiWs.requireAuth = 0;
    c->xiaozhiWs.agentReplyTimeoutMs = 120000;
    c->xiaozhiWs.maxPayloadBytes = 262144;
    c->xiaozhiWs.workerThreads = 8;
    c->xiaozhiWs.maxConnections = 64;
    c->xiaozhiWs.idleTimeoutMs = 300000;
    copy_text(c->xiaozhiWs.asr.provider, sizeof c->xiaozhiWs.asr.provider, "siliconflow");
    copy_text(c->xiaozhiWs.asr.stubInputText, sizeof c->xiaozhiWs.asr.stubInputText,
              "使用 PLC_Diagnosis 检查当前活动告警并给出处理建议");
    c->xiaozhiWs.asr.timeoutMs = 60000;
    c->xiaozhiWs.asr.maxAudioSeconds = 60;
    c->xiaozhiWs.asr.fallbackToStub = 1;
    c->xiaozhiWs.mcpEnabled = 1;
    c->xiaozhiWs.tts.enabled = 1;
    c->xiaozhiWs.tts.timeoutMs = 30000;
    c->xiaozhiWs.tts.sampleRate = 24000;
    c->xiaozhiWs.tts.frameMs = 60;
}
static int load_base(LiteCrabAppConfig* c, const char* path, char* error, size_t errorSize) {
    ParsedFile p;
    if (parse_file(path, &p, error, errorSize))
        return -1;
    if (!p.json)
        return 0;
    int server = object_get(&p, 0, "server");
    if (server >= 0) {
        get_string(&p, server, "listen_ip", c->server.listenIp, sizeof c->server.listenIp);
        get_int(&p, server, "listen_port", &c->server.listenPort);
        get_int(&p, server, "backlog", &c->server.backlog);
        get_int(&p, server, "recv_timeout_ms", &c->server.recvTimeoutMs);
        get_int(&p, server, "send_timeout_ms", &c->server.sendTimeoutMs);
        get_int(&p, server, "max_req_bytes", &c->server.maxReqBytes);
        get_int(&p, server, "worker_threads", &c->server.workerThreads);
        get_int(&p, server, "max_connections", &c->server.maxConnections);
        int aur = object_get(&p, server, "allow_unauthenticated_remote");
        if (aur >= 0) {
            int bv;
            if (!get_bool(&p, server, "allow_unauthenticated_remote", &bv)) {
                set_error(error,
                          errorSize,
                          "allow_unauthenticated_remote must be a JSON boolean: %s",
                          path ? path : "");
                parsed_clear(&p);
                return -1;
            }
            c->server.allowUnauthenticatedRemote = bv;
        }
    }
    int log = object_get(&p, 0, "log");
    if (log >= 0) {
        if (!get_string(&p, log, "dir", c->logDir, sizeof c->logDir)) {
            char file[512];
            if (get_string(&p, log, "filename", file, sizeof file)) {
                char* slash = strrchr(file, '/');
                if (slash) {
                    *slash = 0;
                    copy_text(c->logDir, sizeof c->logDir, file);
                }
            }
        }
    }
    get_string(&p, 0, "workspace_root", c->workspaceRoot, sizeof c->workspaceRoot);
    int xz = object_get(&p, 0, "xiaozhi_ws");
    if (xz >= 0) {
        if (p.tokens[xz].type != LJ_OBJECT) {
            set_error(error, errorSize, "xiaozhi_ws must be an object: %s", path ? path : "");
            parsed_clear(&p);
            return -1;
        }
        get_string(&p, xz, "listen_ip", c->xiaozhiWs.listenIp, sizeof c->xiaozhiWs.listenIp);
        get_int(&p, xz, "listen_port", &c->xiaozhiWs.listenPort);
        get_string(&p, xz, "advertised_ip", c->xiaozhiWs.advertisedIp, sizeof c->xiaozhiWs.advertisedIp);
        get_int(&p, xz, "agent_reply_timeout_ms", &c->xiaozhiWs.agentReplyTimeoutMs);
        get_int(&p, xz, "max_payload_bytes", &c->xiaozhiWs.maxPayloadBytes);
        get_int(&p, xz, "worker_threads", &c->xiaozhiWs.workerThreads);
        get_int(&p, xz, "max_connections", &c->xiaozhiWs.maxConnections);
        get_int(&p, xz, "idle_timeout_ms", &c->xiaozhiWs.idleTimeoutMs);
        get_string(&p, xz, "auth_token", c->xiaozhiWs.authToken, sizeof c->xiaozhiWs.authToken);
        int flag;
        if (get_bool(&p, xz, "enabled", &flag))
            c->xiaozhiWs.enabled = flag;
        if (get_bool(&p, xz, "require_auth", &flag))
            c->xiaozhiWs.requireAuth = flag;
        int asr = object_get(&p, xz, "asr");
        if (asr >= 0 && p.tokens[asr].type == LJ_OBJECT) {
            get_string(&p, asr, "provider", c->xiaozhiWs.asr.provider, sizeof c->xiaozhiWs.asr.provider);
            get_string(&p, asr, "endpoint", c->xiaozhiWs.asr.endpoint, sizeof c->xiaozhiWs.asr.endpoint);
            get_string(&p, asr, "model", c->xiaozhiWs.asr.model, sizeof c->xiaozhiWs.asr.model);
            get_string(&p, asr, "api_key", c->xiaozhiWs.asr.apiKey, sizeof c->xiaozhiWs.asr.apiKey);
            get_string(&p, asr, "stub_input_text", c->xiaozhiWs.asr.stubInputText,
                       sizeof c->xiaozhiWs.asr.stubInputText);
            get_int(&p, asr, "timeout_ms", &c->xiaozhiWs.asr.timeoutMs);
            get_int(&p, asr, "max_audio_seconds", &c->xiaozhiWs.asr.maxAudioSeconds);
            if (get_bool(&p, asr, "fallback_to_stub", &flag))
                c->xiaozhiWs.asr.fallbackToStub = flag;
        }
        if (get_bool(&p, xz, "mcp_enabled", &flag))
            c->xiaozhiWs.mcpEnabled = flag;
        int tts = object_get(&p, xz, "tts");
        if (tts >= 0 && p.tokens[tts].type == LJ_OBJECT) {
            get_string(&p, tts, "endpoint", c->xiaozhiWs.tts.endpoint, sizeof c->xiaozhiWs.tts.endpoint);
            get_string(&p, tts, "model", c->xiaozhiWs.tts.model, sizeof c->xiaozhiWs.tts.model);
            get_string(&p, tts, "voice", c->xiaozhiWs.tts.voice, sizeof c->xiaozhiWs.tts.voice);
            get_string(&p, tts, "api_key", c->xiaozhiWs.tts.apiKey, sizeof c->xiaozhiWs.tts.apiKey);
            get_int(&p, tts, "timeout_ms", &c->xiaozhiWs.tts.timeoutMs);
            get_int(&p, tts, "sample_rate", &c->xiaozhiWs.tts.sampleRate);
            get_int(&p, tts, "frame_ms", &c->xiaozhiWs.tts.frameMs);
            if (get_bool(&p, tts, "enabled", &flag))
                c->xiaozhiWs.tts.enabled = flag;
        }
    }
    parsed_clear(&p);
    return 0;
}
static int load_llm(LiteCrabAppConfig* c,
                    const char* path,
                    const char* providerOverride,
                    char* error,
                    size_t errorSize) {
    ParsedFile p;
    if (parse_file(path, &p, error, errorSize))
        return -1;
    if (!p.json)
        return 0;
    int llm = object_get(&p, 0, "llm");
    if (llm < 0)
        llm = 0;
    if (p.tokens[llm].type != LJ_OBJECT) {
        set_error(error, errorSize, "LLM config must be an object: %s", path);
        parsed_clear(&p);
        return -1;
    }
    char selected[128] = "";
    if (providerOverride && *providerOverride)
        copy_text(selected, sizeof selected, providerOverride);
    else
        get_string(&p, llm, "default", selected, sizeof selected);
    int provider = selected[0] ? object_get(&p, llm, selected) : llm;
    if (provider < 0) {
        set_error(error, errorSize, "unknown default LLM provider: %s", selected);
        parsed_clear(&p);
        return -1;
    }
    if (p.tokens[provider].type != LJ_OBJECT) {
        set_error(error, errorSize, "LLM provider must be an object: %s", selected);
        parsed_clear(&p);
        return -1;
    }
    if (!get_string(&p, provider, "baseUrl", c->llmBaseUrl, sizeof c->llmBaseUrl))
        get_string(&p, provider, "url", c->llmBaseUrl, sizeof c->llmBaseUrl);
    if (!get_string(&p, provider, "apiKey", c->llmApiKey, sizeof c->llmApiKey))
        get_string(&p, provider, "api_key", c->llmApiKey, sizeof c->llmApiKey);
    char apiKeyEnv[128] = "";
    if (get_string(&p, provider, "apiKeyEnv", apiKeyEnv, sizeof apiKeyEnv)) {
        const char* value = getenv(apiKeyEnv);
        if (!value || !*value) {
            set_error(
                error, errorSize, "LLM API key environment variable is not set: %s", apiKeyEnv);
            parsed_clear(&p);
            return -1;
        }
        copy_text(c->llmApiKey, sizeof c->llmApiKey, value);
    }
    if (!get_string(&p, provider, "modelName", c->llmModel, sizeof c->llmModel))
        get_string(&p, provider, "model", c->llmModel, sizeof c->llmModel);
    int setting = object_get(&p, llm, "max_tokens");
    if (setting >= 0 && !get_int(&p, llm, "max_tokens", &c->llm.maxTokens))
        goto invalid_setting;
    setting = object_get(&p, llm, "temperature");
    if (setting >= 0 && !get_double(&p, llm, "temperature", &c->llm.temperature))
        goto invalid_setting;
    setting = object_get(&p, llm, "stream");
    if (setting >= 0 && !get_bool(&p, llm, "stream", &c->llm.stream))
        goto invalid_setting;
    setting = object_get(&p, llm, "timeout_ms");
    if (setting >= 0 && !get_int(&p, llm, "timeout_ms", &c->llm.timeoutMs))
        goto invalid_setting;
    get_string(&p, llm, "reasoning_effort", c->reasoningEffort, sizeof c->reasoningEffort);
    parsed_clear(&p);
    return 0;
invalid_setting:
    set_error(error, errorSize, "invalid LLM setting in config: %s", path);
    parsed_clear(&p);
    return -1;
}
static void env_text(const char* name, char* out, size_t size) {
    const char* v = getenv(name);
    if (v && *v)
        copy_text(out, size, v);
}
static int validate(LiteCrabAppConfig* c, char* error, size_t errorSize) {
    char bindError[256];
    if (RequestServerValidateBindPolicy(&c->server, bindError, sizeof bindError)) {
        set_error(error, errorSize, "%s", bindError);
        return -1;
    }
    if (c->server.listenPort < 1 || c->server.listenPort > 65535) {
        set_error(error, errorSize, "invalid listen port: %s", "");
        return -1;
    }
    if (c->server.backlog < 1 || c->server.maxReqBytes < 1 ||
        c->server.maxReqBytes > 16 * 1024 || c->server.recvTimeoutMs < 100 ||
        c->server.recvTimeoutMs > 600000 || c->server.sendTimeoutMs < 100 ||
        c->server.sendTimeoutMs > 600000 || c->server.workerThreads < 1 ||
        c->server.workerThreads > 8 || c->server.maxConnections < c->server.workerThreads ||
        c->server.maxConnections > 64) {
        set_error(error, errorSize, "invalid server limits: %s", "");
        return -1;
    }
    if (c->xiaozhiWs.enabled && c->xiaozhiWs.listenPort == c->server.listenPort &&
        !strcmp(c->xiaozhiWs.listenIp, c->server.listenIp)) {
        set_error(error, errorSize, "xiaozhi_ws and tcp server share the same bind address: %s", "");
        return -1;
    }
    if (c->xiaozhiWs.enabled && XiaozhiWsValidate(&c->xiaozhiWs, bindError, sizeof bindError)) {
        set_error(error, errorSize, "%s", bindError);
        return -1;
    }
    if (strncmp(c->llmBaseUrl, "http://", 7) && strncmp(c->llmBaseUrl, "https://", 8)) {
        set_error(error, errorSize, "invalid LLM URL: %s", c->llmBaseUrl);
        return -1;
    }
    if (!c->llmModel[0]) {
        set_error(error, errorSize, "missing LLM model: %s", "");
        return -1;
    }
    if (c->llm.maxTokens < 1 || c->llm.maxTokens > 1024 * 1024 || c->llm.timeoutMs < 1 ||
        c->llm.timeoutMs > 60 * 60 * 1000 || c->llm.temperature < 0.0 || c->llm.temperature > 2.0) {
        set_error(error, errorSize, "invalid LLM limits: %s", "");
        return -1;
    }
    return 0;
}
int LiteCrabConfigLoadProvider(LiteCrabAppConfig* c,
                               const char* basePath,
                               const char* llmPath,
                               const char* llmProvider,
                               char* error,
                               size_t errorSize) {
    if (!c)
        return -1;
    LiteCrabConfigDefaults(c);
    if (error && errorSize)
        error[0] = 0;
    if (basePath && *basePath && load_base(c, basePath, error, errorSize))
        return -1;
    if (llmPath && *llmPath && load_llm(c, llmPath, llmProvider, error, errorSize))
        return -1;
    env_text("LITECRAB_WORKSPACE", c->workspaceRoot, sizeof c->workspaceRoot);
    env_text("LITECRAB_LOG_DIR", c->logDir, sizeof c->logDir);
    env_text("LITECRAB_BASE_URL", c->llmBaseUrl, sizeof c->llmBaseUrl);
    env_text("LITECRAB_API_KEY", c->llmApiKey, sizeof c->llmApiKey);
    env_text("LITECRAB_MODEL", c->llmModel, sizeof c->llmModel);
    env_text("LITECRAB_REASONING_EFFORT", c->reasoningEffort, sizeof c->reasoningEffort);
    env_text("LITECRAB_XIAOZHI_WS_AUTH_TOKEN", c->xiaozhiWs.authToken, sizeof c->xiaozhiWs.authToken);
    const char* port = getenv("LITECRAB_PORT");
    if (port && *port)
        c->server.listenPort = atoi(port);
    port = getenv("LITECRAB_XIAOZHI_WS_PORT");
    if (port && *port)
        c->xiaozhiWs.listenPort = atoi(port);
    c->llm.baseUrl = c->llmBaseUrl;
    c->llm.apiKey = c->llmApiKey;
    c->llm.model = c->llmModel;
    c->llm.reasoningEffort = c->reasoningEffort;
    return validate(c, error, errorSize);
}
int LiteCrabConfigLoad(LiteCrabAppConfig* c,
                       const char* basePath,
                       const char* llmPath,
                       char* error,
                       size_t errorSize) {
    return LiteCrabConfigLoadProvider(c, basePath, llmPath, NULL, error, errorSize);
}
