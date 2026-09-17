#include "litecrab/asr_sf.h"
#include "litecrab/json.h"
#include "litecrab/observability.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef LITECRAB_HAVE_OPUS
#include <opus/opus.h>
#endif

#define ASR_HTTP_HEADER_MAX (16 * 1024)
#define ASR_HTTP_BODY_MAX (64 * 1024)
#define ASR_HTTP_REQUEST_MAX (2 * 1024 * 1024)
#define ASR_MULTIPART_BOUNDARY "----litecrab-asr-9f2c3a"
#define ASR_FRAMES_PER_SECOND 17
#define ASR_OPUS_BYTES_PER_SECOND (8 * 1024)

static XiaozhiAsrConfig g_asr;

int AsrSfHaveOpusDecode(void) {
#ifdef LITECRAB_HAVE_OPUS
    return 1;
#else
    return 0;
#endif
}

int AsrSfInit(const XiaozhiAsrConfig* config) {
    if (!config)
        return -1;
    g_asr = *config;
    if (!g_asr.provider[0])
        snprintf(g_asr.provider, sizeof g_asr.provider, "siliconflow");
    if (!g_asr.endpoint[0])
        snprintf(g_asr.endpoint, sizeof g_asr.endpoint, "%s", ASR_SF_ENDPOINT_DEFAULT);
    if (!g_asr.model[0])
        snprintf(g_asr.model, sizeof g_asr.model, "%s", ASR_SF_MODEL_DEFAULT);
    if (!g_asr.apiKey[0])
        snprintf(g_asr.apiKey, sizeof g_asr.apiKey, "%s", ASR_SF_API_KEY_DEFAULT);
    if (g_asr.timeoutMs <= 0)
        g_asr.timeoutMs = 60000;
    if (g_asr.maxAudioSeconds <= 0)
        g_asr.maxAudioSeconds = 60;
    return 0;
}

int AsrSfBuildWavHeader(unsigned char* hdr, uint32_t dataBytes, int sampleRate, int channels, int bits) {
    if (!hdr || sampleRate <= 0 || channels <= 0 || bits <= 0)
        return -1;
    uint32_t byteRate = (uint32_t) sampleRate * channels * bits / 8;
    uint16_t blockAlign = (uint16_t) (channels * bits / 8);
    memcpy(hdr, "RIFF", 4);
    uint32_t riffSize = 36 + dataBytes;
    memcpy(hdr + 4, &riffSize, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmtSize = 16;
    memcpy(hdr + 16, &fmtSize, 4);
    uint16_t audioFormat = 1;
    memcpy(hdr + 20, &audioFormat, 2);
    memcpy(hdr + 22, &channels, 2);
    memcpy(hdr + 24, &sampleRate, 4);
    memcpy(hdr + 28, &byteRate, 4);
    memcpy(hdr + 32, &blockAlign, 2);
    memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &dataBytes, 4);
    return 0;
}

size_t AsrSfBuildMultipartBody(char* out,
                               size_t cap,
                               const char* boundary,
                               const char* model,
                               const unsigned char* wav,
                               size_t wavLen) {
    if (!out || !boundary || !model || (!wav && wavLen))
        return 0;
    size_t used = 0;
    int rc = snprintf(out + used, cap - used,
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"model\"\r\n"
                      "\r\n"
                      "%s\r\n"
                      "--%s\r\n"
                      "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
                      "Content-Type: audio/wav\r\n"
                      "\r\n",
                      boundary,
                      model,
                      boundary);
    if (rc < 0 || (size_t) rc >= cap - used)
        return 0;
    used += (size_t) rc;
    if (wavLen + 64 > cap - used)
        return 0;
    memcpy(out + used, wav, wavLen);
    used += wavLen;
    rc = snprintf(out + used, cap - used, "\r\n--%s--\r\n", boundary);
    if (rc < 0 || (size_t) rc >= cap - used)
        return 0;
    used += (size_t) rc;
    return used;
}

int AsrSfParseTranscriptionResponse(const char* body, char* text, size_t textSize) {
    if (!body || !text || !textSize)
        return -1;
    text[0] = 0;
    if (LjValidate(body, LJ_OBJECT))
        return -1;
    size_t cap = strlen(body) / 2 + 64;
    LjToken* tokens = calloc(cap, sizeof *tokens);
    if (!tokens)
        return -1;
    LjParser parser;
    LjInit(&parser);
    int count = LjParse(&parser, body, strlen(body), tokens, (unsigned) cap);
    int rc = -1;
    if (count > 0) {
        int v = LjObjectGet(body, tokens, count, 0, "text");
        if (v >= 0 && tokens[v].type == LJ_STRING)
            rc = LjString(body, &tokens[v], text, textSize) ? -1 : 0;
    }
    free(tokens);
    return rc;
}

/* ---- raw-socket + OpenSSL HTTPS client (pattern from src/kernel/llm.c) ---- */
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct ssl_method_st SSL_METHOD;
typedef struct x509_verify_param_st X509_VERIFY_PARAM;
extern const SSL_METHOD* TLS_client_method(void);
extern SSL_CTX* SSL_CTX_new(const SSL_METHOD*);
extern void SSL_CTX_free(SSL_CTX*);
extern int SSL_CTX_set_default_verify_paths(SSL_CTX*);
extern void SSL_CTX_set_verify(SSL_CTX*, int, int (*)(int, void*));
extern SSL* SSL_new(SSL_CTX*);
extern void SSL_free(SSL*);
extern int SSL_set_fd(SSL*, int);
extern long SSL_ctrl(SSL*, int, long, void*);
extern X509_VERIFY_PARAM* SSL_get0_param(SSL*);
extern int X509_VERIFY_PARAM_set1_host(X509_VERIFY_PARAM*, const char*, size_t);
extern int SSL_connect(SSL*);
extern long SSL_get_verify_result(const SSL*);
extern int SSL_shutdown(SSL*);
extern int SSL_write(SSL*, const void*, int);
extern int SSL_read(SSL*, void*, int);
#define SSL_VERIFY_PEER 0x01
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define TLSEXT_NAMETYPE_host_name 0

typedef struct {
    int fd;
    SSL_CTX* ctx;
    SSL* ssl;
} AsrConn;

static void asr_conn_close(AsrConn* c) {
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

static int64_t asr_monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int asr_parse_url(const char* u,
                         char* host,
                         size_t hz,
                         int* port,
                         char* path,
                         size_t pz,
                         int* tls) {
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
    snprintf(path, pz, "%s", slash ? slash : "/");
    return 0;
}

static int asr_conn_open(AsrConn* c, const char* host, int port, int useTls, int timeoutMs) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    char portText[16];
    snprintf(portText, sizeof portText, "%d", port);
    struct addrinfo hints = {0}, *res = NULL, *a;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, portText, &hints, &res))
        return -201;
    for (a = res; a; a = a->ai_next) {
        c->fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (c->fd < 0)
            continue;
        struct timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
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
    if (useTls) {
        c->ctx = SSL_CTX_new(TLS_client_method());
        if (!c->ctx) {
            asr_conn_close(c);
            return -201;
        }
        SSL_CTX_set_default_verify_paths(c->ctx);
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
        c->ssl = SSL_new(c->ctx);
        if (!c->ssl) {
            asr_conn_close(c);
            return -201;
        }
        SSL_ctrl(c->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, (void*) host);
        X509_VERIFY_PARAM* param = SSL_get0_param(c->ssl);
        if (!param || !X509_VERIFY_PARAM_set1_host(param, host, 0)) {
            asr_conn_close(c);
            return -201;
        }
        SSL_set_fd(c->ssl, c->fd);
        if (SSL_connect(c->ssl) != 1 || SSL_get_verify_result(c->ssl) != 0) {
            asr_conn_close(c);
            return -201;
        }
    }
    return 0;
}

static ssize_t asr_io_write(AsrConn* c, const void* d, size_t n) {
    return c->ssl ? SSL_write(c->ssl, d, (int) n) : send(c->fd, d, n, 0);
}

static ssize_t asr_io_read(AsrConn* c, void* d, size_t n) {
    return c->ssl ? SSL_read(c->ssl, d, (int) n) : recv(c->fd, d, n, 0);
}

static int asr_send_all(AsrConn* c, const char* d, size_t n) {
    while (n) {
        ssize_t w = asr_io_write(c, d, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        d += w;
        n -= (size_t) w;
    }
    return 0;
}

static int asr_http_post(const char* url,
                         const char* apiKey,
                         const char* body,
                         size_t bodyLen,
                         int timeoutMs,
                         char* resp,
                         size_t respSize,
                         int* statusCode) {
    char host[256], path[512];
    int port, useTls;
    if (asr_parse_url(url, host, sizeof host, &port, path, sizeof path, &useTls))
        return -203;
    AsrConn conn;
    if (asr_conn_open(&conn, host, port, useTls, timeoutMs))
        return -201;
    char head[2048];
    int headLen = snprintf(head,
                           sizeof head,
                           "POST %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Authorization: Bearer %s\r\n"
                           "Content-Type: multipart/form-data; boundary=%s\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           path,
                           host,
                           apiKey,
                           ASR_MULTIPART_BOUNDARY,
                           bodyLen);
    if (headLen <= 0 || (size_t) headLen >= sizeof head) {
        asr_conn_close(&conn);
        return -203;
    }
    if (asr_send_all(&conn, head, (size_t) headLen) || asr_send_all(&conn, body, bodyLen)) {
        LogPrint("[asr] request send failed bodyLen=%zu", bodyLen);
        asr_conn_close(&conn);
        return -204;
    }
    char* headerBuf = malloc(ASR_HTTP_HEADER_MAX + 1);
    if (!headerBuf) {
        asr_conn_close(&conn);
        return -200;
    }
    size_t got = 0;
    int headerDone = 0;
    while (got < ASR_HTTP_HEADER_MAX) {
        ssize_t r = asr_io_read(&conn, headerBuf + got, ASR_HTTP_HEADER_MAX - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            LogPrint("[asr] header read stopped r=%zd got=%zu errno=%d", r, got, errno);
            break;
        }
        got += (size_t) r;
        headerBuf[got] = 0;
        if (memmem(headerBuf, got, "\r\n\r\n", 4)) {
            headerDone = 1;
            break;
        }
    }
    int rc = -203;
    long contentLength = -1;
    int chunked = 0;
    if (headerDone) {
        /* Locate the body separator BEFORE scanning header lines: strtok-style
         * tokenization would overwrite the \r\n bytes the separator is made of. */
        char* sep = memmem(headerBuf, got, "\r\n\r\n", 4);
        char* bodyStart = sep ? sep + 4 : headerBuf + got;
        size_t bodyHave = got - (size_t) (bodyStart - headerBuf);
        *statusCode = (int) strtol(headerBuf + 9, NULL, 10);
        const char* line = headerBuf;
        while (line < (sep ? sep : headerBuf + got)) {
            const char* eol = memchr(line, '\r', (size_t) ((sep ? sep : headerBuf + got) - line));
            if (!eol)
                break;
            if (eol - line > 15 && !strncasecmp(line, "Content-Length:", 15))
                contentLength = strtol(line + 15, NULL, 10);
            else if (eol - line > 18 && !strncasecmp(line, "Transfer-Encoding:", 18) &&
                     strcasestr(line, "chunked"))
                chunked = 1;
            line = eol + 2;
        }
        size_t bodyLen2 = 0;
        if (contentLength >= 0 && (size_t) contentLength >= respSize) {
            rc = -206;
        } else if (chunked) {
            /* read the remaining wire data, then decode chunked in place */
            size_t wireCap = bodyHave + 32 * 1024;
            char* wire = malloc(wireCap + 1);
            if (wire) {
                memcpy(wire, bodyStart, bodyHave);
                size_t wireLen = bodyHave;
                while (wireLen < wireCap) {
                    ssize_t r = asr_io_read(&conn, wire + wireLen, wireCap - wireLen);
                    if (r < 0 && errno == EINTR)
                        continue;
                    if (r <= 0)
                        break;
                    wireLen += (size_t) r;
                }
                char* w = wire;
                char* wEnd = wire + wireLen;
                while (bodyLen2 + 1 < respSize) {
                    char* eol = memchr(w, "\r\n"[0], (size_t) (wEnd - w));
                    if (!eol || eol + 1 >= wEnd)
                        break;
                    long chunkLen = strtol(w, NULL, 16);
                    if (chunkLen <= 0)
                        break;
                    if (wEnd - (eol + 2) < chunkLen)
                        break;
                    if (bodyLen2 + (size_t) chunkLen >= respSize)
                        break;
                    memcpy(resp + bodyLen2, eol + 2, (size_t) chunkLen);
                    bodyLen2 += (size_t) chunkLen;
                    w = eol + 2 + chunkLen;
                    if (wEnd - w >= 2)
                        w += 2;
                }
                resp[bodyLen2] = 0;
                rc = 0;
                free(wire);
            }
        } else {
            long want = contentLength >= 0 ? contentLength : (long) bodyHave;
            while (bodyHave < (size_t) want && bodyHave + 1 < respSize) {
                ssize_t r = asr_io_read(&conn, resp + bodyHave, respSize - 1 - bodyHave);
                if (r < 0 && errno == EINTR)
                    continue;
                if (r <= 0)
                    break;
                bodyHave += (size_t) r;
            }
            memcpy(resp, bodyStart, bodyHave < respSize - 1 ? bodyHave : respSize - 1);
            bodyLen2 = bodyHave;
            resp[bodyLen2] = 0;
            /* Only accept when the promised body actually arrived: a peer that
             * closed early (or a truncated read) must not surface as an empty
             * 2xx response. */
            rc = bodyHave >= (size_t) want && bodyLen2 ? 0 : -205;
        }
    }
    free(headerBuf);
    asr_conn_close(&conn);
    return rc;
}

/* ---- providers ---- */

typedef struct {
    XiaozhiAsrProvider base;
    unsigned char* frameBuf;
    size_t frameLen, frameCap;
    size_t totalBytes;
    size_t maxBytes;
} SfProvider;

static void sf_begin(XiaozhiAsrProvider* p, const char* session_id) {
    (void) session_id;
    SfProvider* sf = (SfProvider*) p;
    sf->frameLen = 0;
    sf->totalBytes = 0;
}

static void sf_feed(XiaozhiAsrProvider* p, const unsigned char* opus, size_t len) {
    SfProvider* sf = (SfProvider*) p;
    if (!opus || !len)
        return;
    sf->totalBytes += len;
    if (sf->totalBytes > sf->maxBytes)
        return;
    if (sf->frameLen + len + 2 > sf->frameCap) {
        size_t grow = sf->frameCap ? sf->frameCap * 2 : 64 * 1024;
        while (grow < sf->frameLen + len + 2)
            grow *= 2;
        if (grow > sf->maxBytes + 4096)
            grow = sf->maxBytes + 4096;
        unsigned char* next = realloc(sf->frameBuf, grow);
        if (!next)
            return;
        sf->frameBuf = next;
        sf->frameCap = grow;
    }
    unsigned char prefix[2] = {(unsigned char) (len >> 8), (unsigned char) (len & 0xFF)};
    memcpy(sf->frameBuf + sf->frameLen, prefix, 2);
    sf->frameLen += 2;
    memcpy(sf->frameBuf + sf->frameLen, opus, len);
    sf->frameLen += len;
}

static int sf_decode_to_wav(SfProvider* sf, unsigned char** wavOut, size_t* wavLen) {
    *wavOut = NULL;
    *wavLen = 0;
    if (!sf->frameLen)
        return -1;
#ifdef LITECRAB_HAVE_OPUS
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(16000, 1, &err);
    if (!dec || err != OPUS_OK)
        return -1;
    size_t pcmCap = 1920 + sf->totalBytes * 24;
    if (pcmCap > 16 * 1024 * 1024)
        pcmCap = 16 * 1024 * 1024;
    unsigned char* wav = malloc(44 + pcmCap);
    if (!wav) {
        opus_decoder_destroy(dec);
        return -1;
    }
    size_t pcmLen = 0;
    size_t off = 0;
    int decodeFailures = 0;
    while (off + 2 <= sf->frameLen) {
        size_t frameLen = ((size_t) sf->frameBuf[off] << 8) | sf->frameBuf[off + 1];
        off += 2;
        if (off + frameLen > sf->frameLen)
            break;
        const unsigned char* frame = sf->frameBuf + off;
        off += frameLen;
        if (!frameLen)
            continue;
        opus_int16 pcm[960];
        int samples = opus_decode(dec, frame, (opus_int32) frameLen, pcm, 960, 0);
        if (samples < 0) {
            decodeFailures++;
            continue;
        }
        if (pcmLen + (size_t) samples * 2 > pcmCap)
            break;
        memcpy(wav + 44 + pcmLen, pcm, (size_t) samples * 2);
        pcmLen += (size_t) samples * 2;
    }
    opus_decoder_destroy(dec);
    if (!pcmLen) {
        free(wav);
        return -1;
    }
    AsrSfBuildWavHeader(wav, (uint32_t) pcmLen, 16000, 1, 16);
    *wavOut = wav;
    *wavLen = 44 + pcmLen;
    (void) decodeFailures;
    return 0;
#else
    (void) sf;
    return -1;
#endif
}

static int sf_finish(XiaozhiAsrProvider* p, const char* session_id, char* text, size_t textSize) {
    (void) session_id;
    SfProvider* sf = (SfProvider*) p;
    text[0] = 0;
    if (!sf->frameLen || !sf->totalBytes)
        return 0;
    unsigned char* wav = NULL;
    size_t wavLen = 0;
    if (sf_decode_to_wav(sf, &wav, &wavLen)) {
        LogPrint("[asr] opus decode failed frames_bytes=%zu total=%zu", sf->frameLen, sf->totalBytes);
        if (g_asr.fallbackToStub) {
            snprintf(text, textSize, "%s", g_asr.stubInputText);
            return 1;
        }
        return -1;
    }
    size_t bodyCap = wavLen + 1024;
    char* body = malloc(bodyCap);
    if (!body) {
        free(wav);
        return g_asr.fallbackToStub ? (snprintf(text, textSize, "%s", g_asr.stubInputText), 1) : -1;
    }
    size_t bodyLen = AsrSfBuildMultipartBody(body, bodyCap, ASR_MULTIPART_BOUNDARY, g_asr.model, wav, wavLen);
    free(wav);
    int rc = -1;
    char* resp = malloc(ASR_HTTP_BODY_MAX);
    if (bodyLen && resp) {
        for (int attempt = 0; attempt < 3 && rc; attempt++) {
            if (attempt)
                usleep(100 * 1000); /* transient localhost/VM hiccup backoff */
            int status = 0;
            int httpRc = asr_http_post(g_asr.endpoint,
                                       g_asr.apiKey,
                                       body,
                                       bodyLen,
                                       g_asr.timeoutMs,
                                       resp,
                                       ASR_HTTP_BODY_MAX,
                                       &status);
            if (httpRc == 0 && status >= 200 && status < 300) {
                if (!AsrSfParseTranscriptionResponse(resp, text, textSize)) {
                    rc = 0;
                } else {
                    LogPrint("[asr] transcription response parse failed len=%zu body=%.120s",
                             strlen(resp),
                             resp);
                }
            } else {
                LogPrint("[asr] transcribe http failed rc=%d status=%d attempt=%d endpoint=%.96s",
                         httpRc,
                         status,
                         attempt,
                         g_asr.endpoint);
            }
        }
    }
    free(resp);
    free(body);
    if (rc) {
        if (g_asr.fallbackToStub) {
            snprintf(text, textSize, "%s", g_asr.stubInputText);
            return 1;
        }
        return -1;
    }
    return 0;
}

static void sf_abort(XiaozhiAsrProvider* p) {
    SfProvider* sf = (SfProvider*) p;
    sf->frameLen = 0;
    sf->totalBytes = 0;
}

typedef struct {
    XiaozhiAsrProvider base;
    size_t fedBytes;
} StubProvider;

static void stub_begin(XiaozhiAsrProvider* p, const char* session_id) {
    (void) session_id;
    ((StubProvider*) p)->fedBytes = 0;
}

static void stub_feed(XiaozhiAsrProvider* p, const unsigned char* opus, size_t len) {
    (void) opus;
    ((StubProvider*) p)->fedBytes += len;
}

static int stub_finish(XiaozhiAsrProvider* p, const char* session_id, char* text, size_t textSize) {
    (void) p;
    (void) session_id;
    snprintf(text, textSize, "%s", g_asr.stubInputText);
    return 0;
}

static void stub_abort(XiaozhiAsrProvider* p) {
    (void) p;
}

XiaozhiAsrProvider* AsrSfCreateProvider(void) {
    if (!strcmp(g_asr.provider, "stub")) {
        StubProvider* p = calloc(1, sizeof *p);
        if (!p)
            return NULL;
        p->base.begin = stub_begin;
        p->base.feed_audio = stub_feed;
        p->base.finish = stub_finish;
        p->base.abort = stub_abort;
        return &p->base;
    }
    SfProvider* p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->maxBytes = (size_t) g_asr.maxAudioSeconds * ASR_OPUS_BYTES_PER_SECOND;
    p->base.begin = sf_begin;
    p->base.feed_audio = sf_feed;
    p->base.finish = sf_finish;
    p->base.abort = sf_abort;
    return &p->base;
}

void AsrSfDestroyProvider(XiaozhiAsrProvider* provider) {
    if (!provider)
        return;
    if (!strcmp(g_asr.provider, "stub")) {
        free(provider);
        return;
    }
    SfProvider* sf = (SfProvider*) provider;
    free(sf->frameBuf);
    free(sf);
}
