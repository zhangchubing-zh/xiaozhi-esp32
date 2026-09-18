#include "litecrab/json.h"
#include "litecrab/kernel.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LLM_WIRE_RESPONSE_MAX (256U * 1024U)
#define LLM_HTTP_HEADER_MAX (8U * 1024U)
static LlmConfig cfg;
static _Thread_local int64_t requestDeadlineMs;
static _Thread_local int requestCallsRemaining;
static _Thread_local unsigned long requestCancelEpoch;
static atomic_ulong cancelEpoch;

static int64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int remaining_ms(void) {
    if (requestDeadlineMs > 0 && requestCancelEpoch != atomic_load(&cancelEpoch))
        return 0;
    int64_t remaining = requestDeadlineMs > 0 ? requestDeadlineMs - monotonic_ms() : cfg.timeoutMs;
    if (remaining <= 0)
        return 0;
    if (remaining > cfg.timeoutMs)
        remaining = cfg.timeoutMs;
    return (int) remaining;
}
void LlmRequestScopeBegin(long long deadlineMs, int maxCalls) {
    requestDeadlineMs = deadlineMs;
    requestCallsRemaining = maxCalls;
    requestCancelEpoch = atomic_load(&cancelEpoch);
}
void LlmCancelActiveRequests(void) { atomic_fetch_add(&cancelEpoch, 1); }
void LlmRequestScopeEnd(void) {
    requestDeadlineMs = 0;
    requestCallsRemaining = 0;
}


static char base[768], key[512], model[256], effort[64];
static int
parse_url(const char* u, char* host, size_t hz, int* port, char* path, size_t pz, int* tls) {
    if (!u)
        return -1;
    const char* p;
    if (!strncmp(u, "https://", 8)) {
        *tls = 1;
        *port = 443;
        p = u + 8;
    } else if (!strncmp(u, "http://", 7)) {
        *tls = 0;
        *port = 80;
        p = u + 7;
    } else
        return -1;
    const char *slash = strchr(p, '/'), *end = slash ? slash : p + strlen(p), *colon = NULL;
    for (const char* x = p; x < end; x++)
        if (*x == ':')
            colon = x;
    size_t hn = (size_t) ((colon ? colon : end) - p);
    if (!hn || hn >= hz)
        return -1;
    memcpy(host, p, hn);
    host[hn] = 0;
    if (colon) {
        char b[16];
        size_t n = (size_t) (end - colon - 1);
        if (!n || n >= sizeof b)
            return -1;
        memcpy(b, colon + 1, n);
        b[n] = 0;
        *port = atoi(b);
        if (*port <= 0 || *port > 65535)
            return -1;
    }
    snprintf(path, pz, "%s", slash ? slash : "/v1/chat/completions");
    if (!strcmp(path, "/") || !strcmp(path, "/v1"))
        snprintf(path, pz, "%s/v1/chat/completions", strcmp(path, "/") ? "" : "");
    return 0;
}
int LlmInit(const LlmConfig* c) {
    memset(&cfg, 0, sizeof cfg);
    const char* url = c && c->baseUrl ? c->baseUrl : getenv("LITECRAB_BASE_URL");
    const char *k = c && c->apiKey ? c->apiKey : getenv("LITECRAB_API_KEY"),
               *m = c && c->model ? c->model : getenv("LITECRAB_MODEL");
    if (!url)
        url = "http://127.0.0.1:18080/v1/chat/completions";
    if (!k)
        k = "";
    if (!m)
        m = "litecrab-test";
    snprintf(base, sizeof base, "%s", url);
    snprintf(key, sizeof key, "%s", k);
    snprintf(model, sizeof model, "%s", m);
    snprintf(effort, sizeof effort, "%s", c && c->reasoningEffort ? c->reasoningEffort : "");
    cfg.baseUrl = base;
    cfg.apiKey = key;
    cfg.model = model;
    cfg.reasoningEffort = effort;
    cfg.maxTokens = c && c->maxTokens > 0 ? c->maxTokens : 2048;
    cfg.temperature = c ? c->temperature : 0.2;
    cfg.stream = c ? c->stream : 1;
    cfg.timeoutMs = c && c->timeoutMs > 0 ? c->timeoutMs : 60000;
    if (parse_url(
            base, cfg.host, sizeof cfg.host, &cfg.port, cfg.path, sizeof cfg.path, &cfg.useTls))
        return -1;
    return 0;
}
const LlmConfig* LlmGetConfig(void) {
    return &cfg;
}
static int json_array_has_items(const char* json) {
    const char* p = json;
    while (*p && strchr(" \r\n\t", *p))
        p++;
    if (*p != '[')
        return 0;
    p++;
    while (*p && strchr(" \r\n\t", *p))
        p++;
    return *p != ']';
}
int LlmBuildChatToolsRequestJson(
    const char* messages, const char* tools, int stream, char* out, size_t z) {
    if (!messages || !tools || !out || LjValidate(messages, LJ_ARRAY) ||
        LjValidate(tools, LJ_ARRAY))
        return -1;
    LjBuf b;
    LjBufInit(&b, out, z);
    LjAppend(&b, "{\"model\":");
    LjAppendJsonString(&b, cfg.model);
    LjAppend(&b,
             ",\"messages\":%s",
             messages);
    if (json_array_has_items(tools))
        LjAppend(&b, ",\"tools\":%s,\"tool_choice\":\"auto\"", tools);
    LjAppend(&b,
             ",\"stream\":%s,\"max_tokens\":%d,\"temperature\":%.6g",
             stream ? "true" : "false",
             cfg.maxTokens,
             cfg.temperature);
    if (cfg.reasoningEffort && *cfg.reasoningEffort) {
        LjAppend(&b, ",\"reasoning_effort\":");
        LjAppendJsonString(&b, cfg.reasoningEffort);
    }
    LjAppend(&b, "}");
    return b.failed ? -1 : 0;
}
static int
request_json(const char* system, const char* messages, const char* tools, int stream, char** out) {
    size_t mz = strlen(messages), sz = strlen(system ? system : "") * 6 + 256;
    const char *open = strchr(messages, '['), *close = strrchr(messages, ']');
    if (!open || !close || close < open || LjValidate(messages, LJ_ARRAY))
        return -1;
    char* all = malloc(mz + sz);
    if (!all)
        return -1;
    LjBuf b;
    LjBufInit(&b, all, mz + sz);
    LjAppend(&b, "[");
    if (system && *system) {
        LjAppend(&b, "{\"role\":\"system\",\"content\":");
        LjAppendJsonString(&b, system);
        LjAppend(&b, "}");
    }
    const char* p = open + 1;
    size_t n = (size_t) (close - p);
    while (n && strchr(" \r\n\t", p[n - 1]))
        n--;
    while (n && strchr(" \r\n\t", *p)) {
        p++;
        n--;
    }
    if (n) {
        if (system && *system)
            LjAppend(&b, ",");
        if (b.len + n + 2 >= b.size)
            b.failed = 1;
        else {
            memcpy(b.data + b.len, p, n);
            b.len += n;
            b.data[b.len] = 0;
        }
    }
    LjAppend(&b, "]");
    if (b.failed) {
        free(all);
        return -1;
    }
    size_t cap = strlen(all) + strlen(tools) + strlen(cfg.model) * 6 + 2048;
    char* body = malloc(cap);
    if (!body) {
        free(all);
        return -1;
    }
    int rc = LlmBuildChatToolsRequestJson(all, tools, stream, body, cap);
    free(all);
    if (rc) {
        free(body);
        return -1;
    }
    *out = body;
    return 0;
}
typedef struct {
    int fd;
    SSL_CTX* ctx;
    SSL* ssl;
} Conn;
static void conn_close(Conn* c) {
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
    if (c->ctx)
        SSL_CTX_free(c->ctx);
    if (c->fd >= 0)
        close(c->fd);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}
static int conn_open(Conn* c) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    char port[16];
    snprintf(port, sizeof port, "%d", cfg.port);
    struct addrinfo hints = {0}, *res = NULL, *a;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(cfg.host, port, &hints, &res))
        return -201;
    for (a = res; a; a = a->ai_next) {
        c->fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (c->fd < 0)
            continue;
        int timeout = remaining_ms();
        if (timeout <= 0) {
            close(c->fd);
            c->fd = -1;
            break;
        }
        if (timeout > 2000)
            timeout = 2000;
        struct timeval tv = {timeout / 1000, (timeout % 1000) * 1000};
        setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (!connect(c->fd, a->ai_addr, a->ai_addrlen))
            break;
        close(c->fd);
        c->fd = -1;
    }
    freeaddrinfo(res);
    if (c->fd < 0)
        return -201;
    if (cfg.useTls) {
        c->ctx = SSL_CTX_new(TLS_client_method());
        if (!c->ctx) {
            conn_close(c);
            return -201;
        }
        SSL_CTX_set_default_verify_paths(c->ctx);
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
        c->ssl = SSL_new(c->ctx);
        if (!c->ssl) {
            conn_close(c);
            return -201;
        }
        SSL_ctrl(c->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, (void*) cfg.host);
        X509_VERIFY_PARAM* param = SSL_get0_param(c->ssl);
        if (!param || !X509_VERIFY_PARAM_set1_host(param, cfg.host, 0)) {
            conn_close(c);
            return -201;
        }
        SSL_set_fd(c->ssl, c->fd);
        if (SSL_connect(c->ssl) != 1 || SSL_get_verify_result(c->ssl) != 0) {
            conn_close(c);
            return -201;
        }
    }
    return 0;
}
static ssize_t io_write(Conn* c, const void* d, size_t n) {
    return c->ssl ? SSL_write(c->ssl, d, (int) n) : send(c->fd, d, n, 0);
}
static ssize_t io_read(Conn* c, void* d, size_t n) {
    return c->ssl ? SSL_read(c->ssl, d, (int) n) : recv(c->fd, d, n, 0);
}
static int send_all(Conn* c, const char* d, size_t n) {
    while (n) {
        ssize_t w = io_write(c, d, n);
        if (w <= 0)
            return -1;
        d += w;
        n -= (size_t) w;
    }
    return 0;
}
static int decode_chunked(const char* in, size_t n, char** out) {
    size_t pos = 0, cap = n + 1, len = 0;
    char* b = malloc(cap);
    if (!b)
        return -1;
    while (pos < n) {
        const char* e = strstr(in + pos, "\r\n");
        if (!e) {
            free(b);
            return -1;
        }
        char s[32];
        size_t sn = (size_t) (e - (in + pos));
        if (sn >= sizeof s) {
            free(b);
            return -1;
        }
        memcpy(s, in + pos, sn);
        s[sn] = 0;
        char* semi = strchr(s, ';');
        if (semi)
            *semi = 0;
        char* end;
        unsigned long chunk = strtoul(s, &end, 16);
        if (end == s) {
            free(b);
            return -1;
        }
        pos = (size_t) (e - in) + 2;
        if (!chunk)
            break;
        if (chunk > n - pos || len + chunk >= cap) {
            free(b);
            return -1;
        }
        memcpy(b + len, in + pos, chunk);
        len += chunk;
        pos += chunk;
        if (pos + 2 > n || in[pos] != '\r' || in[pos + 1] != '\n') {
            free(b);
            return -1;
        }
        pos += 2;
    }
    b[len] = 0;
    *out = b;
    return 0;
}
static int http_post(const char* body, char** out) {
    Conn c;
    int rc = conn_open(&c);
    if (rc)
        return rc;
    size_t hz = strlen(body) + strlen(cfg.apiKey) + strlen(cfg.host) + strlen(cfg.path) + 512;
    char* req = malloc(hz);
    if (!req) {
        conn_close(&c);
        return -200;
    }
    int rn =
        snprintf(req,
                 hz,
                 "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nAuthorization: "
                 "Bearer %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                 cfg.path,
                 cfg.host,
                 cfg.apiKey,
                 strlen(body),
                 body);
    if (rn < 0 || (size_t) rn >= hz || send_all(&c, req, (size_t) rn)) {
        free(req);
        conn_close(&c);
        return -201;
    }
    free(req);
    size_t cap = 65536, n = 0;
    char* resp = malloc(cap);
    if (!resp) {
        conn_close(&c);
        return -200;
    }
    for (;;) {
        if (!remaining_ms()) {
            free(resp);
            conn_close(&c);
            return -205;
        }
        if (cap - n < 8192) {
            if (cap >= LLM_WIRE_RESPONSE_MAX) {
                free(resp);
                conn_close(&c);
                return -206;
            }
            cap *= 2;
            if (cap > LLM_WIRE_RESPONSE_MAX)
                cap = LLM_WIRE_RESPONSE_MAX;
            char* q = realloc(resp, cap);
            if (!q) {
                free(resp);
                conn_close(&c);
                return -200;
            }
            resp = q;
        }
        ssize_t got = io_read(&c, resp + n, cap - n - 1);
        if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (got <= 0)
            break;
        n += (size_t) got;
        resp[n] = 0;
        char* headerEnd = strstr(resp, "\r\n\r\n");
        if (!headerEnd && n > LLM_HTTP_HEADER_MAX) {
            free(resp);
            conn_close(&c);
            return -206;
        }
        if (headerEnd) {
            size_t headerBytes = (size_t) (headerEnd + 4 - resp);
            if (headerBytes > LLM_HTTP_HEADER_MAX) {
                free(resp);
                conn_close(&c);
                return -206;
            }
            char* lengthHeader = strcasestr(resp, "Content-Length:");
            if (lengthHeader && lengthHeader < headerEnd) {
                lengthHeader += 15;
                while (*lengthHeader == ' ' || *lengthHeader == '\t')
                    lengthHeader++;
                errno = 0;
                char* lengthEnd = NULL;
                unsigned long long declared = strtoull(lengthHeader, &lengthEnd, 10);
                if (errno || lengthEnd == lengthHeader || declared > LLM_WIRE_RESPONSE_MAX - headerBytes) {
                    free(resp);
                    conn_close(&c);
                    return -206;
                }
            }
        }
    }
    conn_close(&c);
    resp[n] = 0;
    char* sep = strstr(resp, "\r\n\r\n");
    if (!sep) {
        free(resp);
        return -203;
    }
    int status = 0;
    sscanf(resp, "HTTP/%*s %d", &status);
    if (status < 200 || status >= 300) {
        free(resp);
        if (status == 413) return -206;
        if (status == 429 || status == 503) return -208;
        if (status == 500 || status == 502 || status == 504) return -209;
        return -207;
    }
    char* payload = sep + 4;
    size_t available = n - (size_t) (payload - resp);
    if (strcasestr(resp, "Transfer-Encoding: chunked")) {
        char* decoded = NULL;
        if (decode_chunked(payload, available, &decoded)) {
            free(resp);
            return -203;
        }
        free(resp);
        *out = decoded;
    } else {
        size_t wanted = available;
        char* cl = strcasestr(resp, "Content-Length:");
        if (cl && cl < sep) {
            cl += 15;
            while (*cl == ' ' || *cl == '\t')
                cl++;
            char* end;
            unsigned long long value = strtoull(cl, &end, 10);
            if (end == cl || value > SIZE_MAX || value > available) {
                free(resp);
                return -203;
            }
            wanted = (size_t) value;
        }
        char* copy = malloc(wanted + 1);
        if (!copy) {
            free(resp);
            return -200;
        }
        memcpy(copy, payload, wanted);
        copy[wanted] = 0;
        free(resp);
        *out = copy;
    }
    return 0;
}
static int token_dup(const char* j, const LjToken* t, char** out) {
    if (t->type != LJ_STRING)
        return -1;
    size_t z = (size_t) (t->end - t->start) + 1;
    char* s = malloc(z);
    if (!s)
        return -1;
    if (LjString(j, t, s, z)) {
        free(s);
        return -1;
    }
    *out = s;
    return 0;
}
static int append_dyn(char** p, size_t* len, size_t* capacity, const char* s) {
    size_t add = strlen(s);
    if (add > LLM_WIRE_RESPONSE_MAX - *len)
        return -1;
    size_t needed = *len + add + 1;
    size_t next = *capacity ? *capacity : 256;
    while (next < needed) {
        if (next >= LLM_WIRE_RESPONSE_MAX)
            return -1;
        next *= 2;
        if (next > LLM_WIRE_RESPONSE_MAX)
            next = LLM_WIRE_RESPONSE_MAX;
    }
    char* q = *capacity >= needed ? *p : realloc(*p, next);
    if (!q)
        return -1;
    if (*capacity < needed)
        *capacity = next;
    memcpy(q + *len, s, add + 1);
    *p = q;
    *len += add;
    return 0;
}
static int
parse_tool_array(const char* j, LjToken* t, int n, int arr, LlmResponse* r, int streaming) {
    for (int k = 0; k < t[arr].size; k++) {
        int tc = LjArrayGet(t, n, arr, k);
        if (tc < 0 || t[tc].type != LJ_OBJECT)
            continue;
        int idx = k, iv = LjObjectGet(j, t, n, tc, "index");
        int64_t x;
        if (iv >= 0 && !LjInt64(j, &t[iv], &x))
            idx = (int) x;
        if (idx < 0 || idx >= LLM_MAX_TOOL_CALLS)
            continue;
        if (idx >= r->callCount)
            r->callCount = idx + 1;
        LlmToolCall* c = &r->calls[idx];
        int v = LjObjectGet(j, t, n, tc, "id");
        if (v >= 0 && t[v].type == LJ_STRING) {
            char part[64];
            if (!LjString(j, &t[v], part, sizeof part) && part[0] && (!streaming || !c->id[0]))
                snprintf(c->id, sizeof c->id, "%s", part);
        }
        int fn = LjObjectGet(j, t, n, tc, "function");
        if (fn >= 0) {
            v = LjObjectGet(j, t, n, fn, "name");
            if (v >= 0 && t[v].type == LJ_STRING) {
                char part[64];
                if (!LjString(j, &t[v], part, sizeof part) && part[0]) {
                    if (!streaming || !c->name[0])
                        snprintf(c->name, sizeof c->name, "%s", part);
                    else if (strcmp(c->name, part) &&
                             strlen(c->name) + strlen(part) < sizeof c->name)
                        strcat(c->name, part);
                }
            }
            v = LjObjectGet(j, t, n, fn, "arguments");
            if (v >= 0 && t[v].type == LJ_STRING) {
                char* s = NULL;
                if (token_dup(j, &t[v], &s))
                    return -1;
                if (streaming) {
                    if (append_dyn(&c->input, &c->inputLen, &c->inputCapacity, s)) {
                        free(s);
                        return -1;
                    }
                    free(s);
                } else {
                    free(c->input);
                    c->input = s;
                    c->inputLen = strlen(s);
                }
            }
        }
    }
    return 0;
}
static int parse_event(const char* j, LlmResponse* r, int stream) {
    if (LjValidate(j, LJ_OBJECT))
        return -1;
    LjToken* t;
    int n;
    size_t cap = strlen(j) / 2 + 32;
    t = calloc(cap, sizeof *t);
    if (!t)
        return -1;
    LjParser p;
    LjInit(&p);
    n = LjParse(&p, j, strlen(j), t, (unsigned) cap);
    if (n < 1) {
        free(t);
        return -1;
    }
    int choices = LjObjectGet(j, t, n, 0, "choices"),
        first = choices >= 0 ? LjArrayGet(t, n, choices, 0) : -1,
        msg = first >= 0 ? LjObjectGet(j, t, n, first, stream ? "delta" : "message") : -1;
    if (msg >= 0) {
        int v = LjObjectGet(j, t, n, msg, "content");
        if (v >= 0 && t[v].type == LJ_STRING) {
            char tmp[LLM_RESPONSE_TEXT_MAX];
            if (!LjString(j, &t[v], tmp, sizeof tmp)) {
                size_t left = sizeof r->text - 1 - r->textLen;
                size_t add = strlen(tmp);
                if (add > left) {
                    free(t);
                    return -1;
                }
                memcpy(r->text + r->textLen, tmp, add);
                r->textLen += add;
                r->text[r->textLen] = 0;
            }
        }
        int calls = LjObjectGet(j, t, n, msg, "tool_calls");
        if (calls >= 0 && parse_tool_array(j, t, n, calls, r, stream)) {
            free(t);
            return -1;
        }
        int reasoning = LjObjectGet(j, t, n, msg, "reasoning_content");
        if (reasoning < 0)
            reasoning = LjObjectGet(j, t, n, msg, "reasoning");
        if (reasoning >= 0 && t[reasoning].type == LJ_STRING) {
            int64_t len = (int64_t)(t[reasoning].end - t[reasoning].start);
            if (len > 0)
                r->reasoningTokens += (int)len;
        }
    }
    int usage = LjObjectGet(j, t, n, 0, "usage");
    if (usage >= 0) {
        int64_t v;
        int q = LjObjectGet(j, t, n, usage, "prompt_tokens");
        if (q >= 0 && !LjInt64(j, &t[q], &v))
            r->promptTokens = (int) v;
        q = LjObjectGet(j, t, n, usage, "completion_tokens");
        if (q >= 0 && !LjInt64(j, &t[q], &v))
            r->completionTokens = (int) v;
        q = LjObjectGet(j, t, n, usage, "total_tokens");
        if (q >= 0 && !LjInt64(j, &t[q], &v))
            r->totalTokens = (int) v;
    }
    free(t);
    r->toolUse = r->callCount > 0;
    return choices >= 0 ? 0 : -1;
}
int LlmParseResponseBody(const char* body, int stream, LlmResponse* r) {
    if (!body || !r)
        return -1;
    memset(r, 0, sizeof *r);
    if (!stream) {
        int result = parse_event(body, r, 0);
        if (result)
            LlmResponseClear(r);
        return result;
    }
    const char* p = body;
    int seen = 0, done = 0;
    while (*p) {
        const char* e = strchr(p, '\n');
        size_t n = e ? (size_t) (e - p) : strlen(p);
        while (n && p[n - 1] == '\r')
            n--;
        if (n >= 6 && !strncmp(p, "data:", 5)) {
            const char* d = p + 5;
            while ((size_t) (d - p) < n && *d == ' ')
                d++;
            size_t dn = n - (size_t) (d - p);
            if (dn == 6 && !strncmp(d, "[DONE]", 6)) {
                done = 1;
            } else {
                char* j = malloc(dn + 1);
                if (!j) {
                    LlmResponseClear(r);
                    return -1;
                }
                memcpy(j, d, dn);
                j[dn] = 0;
                if (parse_event(j, r, 1)) {
                    free(j);
                    LlmResponseClear(r);
                    return -1;
                }
                seen = 1;
                free(j);
            }
        }
        if (!e)
            break;
        p = e + 1;
    }
    r->toolUse = r->callCount > 0;
    if (!seen || !done) {
        LlmResponseClear(r);
        return -1;
    }
    return 0;
}
void LlmResponseClear(LlmResponse* r) {
    if (!r)
        return;
    for (int i = 0; i < LLM_MAX_TOOL_CALLS; i++)
        free(r->calls[i].input);
    memset(r, 0, sizeof *r);
}
static int retry_backoff_ms(int retryIndex) {
    int base = 2000 << retryIndex;
    int jitterLimit = base / 4;
    struct timeval now = {0};
    gettimeofday(&now, NULL);
    unsigned seed = (unsigned) now.tv_sec ^ (unsigned) now.tv_usec ^
                    ((unsigned) getpid() << 16) ^ ((unsigned) retryIndex * 2654435761u);
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return base + (int) (seed % (unsigned) (jitterLimit + 1));
}

int LlmChatToolsEx(
    const char* system, const char* messages, const char* tools, LlmResponse* r, int stream) {
    if (!messages || !tools || !r)
        return -200;
    if (requestDeadlineMs > 0 && (!remaining_ms() || requestCallsRemaining <= 0))
        return -210;
    if (requestDeadlineMs > 0)
        requestCallsRemaining--;
    if (stream < 0)
        stream = cfg.stream;
    char* body = NULL;
    if (request_json(system, messages, tools, stream, &body))
        return -200;
    int last = -200;
    enum { MAX_ATTEMPTS = 4 };
    for (int i = 0; i < MAX_ATTEMPTS; i++) {
        char* response = NULL;
        last = http_post(body, &response);
        if (!last) {
            last = LlmParseResponseBody(response, stream, r);
            free(response);
            if (!last)
                break;
            last = -203;
        }
        if (last == -206 || last == -207)
            break;
        if (last == -208 || last == -209) {
            if (i + 1 < MAX_ATTEMPTS) {
                int baseBackoff = 2000 << i;
                int backoff_ms = retry_backoff_ms(i);
                LogPrint("[llm] retryable http error code=%d attempt=%d/%d base_backoff_ms=%d jitter_ms=%d backoff_ms=%d",
                         last,
                         i + 1,
                         MAX_ATTEMPTS,
                         baseBackoff,
                         backoff_ms - baseBackoff,
                         backoff_ms);
                int remaining = remaining_ms();
                if (remaining <= backoff_ms) {
                    last = -205;
                    break;
                }
                usleep((unsigned)(backoff_ms * 1000));
                continue;
            }
            break;
        }
    }
    free(body);
    if (!last && getenv("LITECRAB_DEBUG_LLM"))
        for (int i = 0; i < r->callCount; i++)
            LogPrint("[llm_debug] call=%d name=%s input=%s",
                     i,
                     r->calls[i].name,
                     r->calls[i].input ? r->calls[i].input : "");
    return last;
}
