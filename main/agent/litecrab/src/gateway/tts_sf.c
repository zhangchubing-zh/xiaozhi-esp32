#include "litecrab/json.h"
#include "litecrab/observability.h"
#include "litecrab/tts_sf.h"

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


#define TTS_HTTP_HEADER_MAX (16 * 1024)
#define TTS_WAV_MAX (4 * 1024 * 1024)
#define TTS_MULTIPART_UNUSED 0

static XiaozhiTtsConfig g_tts;

int TtsSfHaveOpusEncode(void) {
#ifdef LITECRAB_HAVE_OPUS
    return 1;
#else
    return 0;
#endif
}

int TtsSfInit(const XiaozhiTtsConfig* config) {
    if (!config)
        return -1;
    g_tts = *config;
    if (!g_tts.endpoint[0])
        snprintf(g_tts.endpoint, sizeof g_tts.endpoint, "%s", TTS_SF_ENDPOINT_DEFAULT);
    if (!g_tts.model[0])
        snprintf(g_tts.model, sizeof g_tts.model, "%s", TTS_SF_MODEL_DEFAULT);
    if (!g_tts.voice[0])
        snprintf(g_tts.voice, sizeof g_tts.voice, "%s", TTS_SF_VOICE_DEFAULT);
    if (!g_tts.apiKey[0])
        snprintf(g_tts.apiKey, sizeof g_tts.apiKey, "%s", TTS_SF_API_KEY_DEFAULT);
    if (g_tts.timeoutMs <= 0)
        g_tts.timeoutMs = 30000;
    if (g_tts.sampleRate != 8000 && g_tts.sampleRate != 12000 && g_tts.sampleRate != 16000 &&
        g_tts.sampleRate != 24000 && g_tts.sampleRate != 48000)
        g_tts.sampleRate = 24000;
    if (g_tts.frameMs != 20 && g_tts.frameMs != 40 && g_tts.frameMs != 60)
        g_tts.frameMs = 60;
    return 0;
}

int TtsSfEnabled(void) {
    return g_tts.enabled && TtsSfHaveOpusEncode();
}

int TtsSfSampleRate(void) {
    return g_tts.sampleRate;
}

int TtsSfFrameMs(void) {
    return g_tts.frameMs;
}

size_t TtsSfBuildSpeechRequestJson(char* out, size_t cap, const char* model,
                                   const char* voice, const char* text) {
    if (!out || !cap || !model || !voice || !text)
        return 0;
    LjBuf b;
    LjBufInit(&b, out, cap);
    LjAppend(&b, "{\"model\":");
    LjAppendJsonString(&b, model);
    LjAppend(&b, ",\"input\":");
    LjAppendJsonString(&b, text);
    LjAppend(&b, ",\"voice\":");
    LjAppendJsonString(&b, voice);
    LjAppend(&b, ",\"response_format\":\"wav\"}");
    return b.failed ? 0 : b.len;
}

int TtsSfParseWavPcm(const unsigned char* wav, size_t wavLen, int* sampleRate,
                     const unsigned char** pcmOut, size_t* pcmLen) {
    if (!wav || wavLen < 44 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4))
        return -1;
    unsigned short channels = 0, bits = 0;
    unsigned int rate = 0;
    memcpy(&channels, wav + 22, 2);
    memcpy(&rate, wav + 24, 4);
    memcpy(&bits, wav + 34, 2);
    if (channels != 1 || bits != 16)
        return -1;
    size_t off = 12;
    while (off + 8 <= wavLen) {
        unsigned int chunkSize;
        memcpy(&chunkSize, wav + off + 4, 4);
        if (!memcmp(wav + off, "data", 4)) {
            if (off + 8 + chunkSize > wavLen)
                chunkSize = (unsigned int) (wavLen - off - 8);
            if (sampleRate)
                *sampleRate = (int) rate;
            if (pcmOut)
                *pcmOut = wav + off + 8;
            if (pcmLen)
                *pcmLen = chunkSize;
            return 0;
        }
        off += 8 + chunkSize + (chunkSize & 1);
    }
    return -1;
}


typedef struct {
    int fd;
    SSL_CTX* ctx;
    SSL* ssl;
} TtsConn;

static void tts_conn_close(TtsConn* c) {
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

static int tts_parse_url(const char* u, char* host, size_t hz, int* port,
                         char* path, size_t pz, int* tls) {
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

static ssize_t tts_io_write(TtsConn* c, const void* d, size_t n) {
    return c->ssl ? SSL_write(c->ssl, d, (int) n) : send(c->fd, d, n, 0);
}

static ssize_t tts_io_read(TtsConn* c, void* d, size_t n) {
    return c->ssl ? SSL_read(c->ssl, d, (int) n) : recv(c->fd, d, n, 0);
}

static int tts_send_all(TtsConn* c, const char* d, size_t n) {
    while (n) {
        ssize_t w = tts_io_write(c, d, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        d += w;
        n -= (size_t) w;
    }
    return 0;
}

static int tts_http_post(const char* url, const char* apiKey, const char* body,
                         size_t bodyLen, int timeoutMs, unsigned char* resp,
                         size_t respCap, size_t* respLen) {
    char host[256], path[512];
    int port, useTls;
    if (tts_parse_url(url, host, sizeof host, &port, path, sizeof path, &useTls))
        return -203;
    TtsConn conn;
    memset(&conn, 0, sizeof conn);
    conn.fd = -1;
    char portText[16];
    snprintf(portText, sizeof portText, "%d", port);
    struct addrinfo hints = {0}, *res = NULL, *a;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, portText, &hints, &res))
        return -201;
    for (a = res; a; a = a->ai_next) {
        conn.fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (conn.fd < 0)
            continue;
        struct timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        setsockopt(conn.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(conn.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (!connect(conn.fd, a->ai_addr, a->ai_addrlen))
            break;
        close(conn.fd);
        conn.fd = -1;
    }
    freeaddrinfo(res);
    if (conn.fd < 0)
        return -201;
    if (useTls) {
        conn.ctx = SSL_CTX_new(TLS_client_method());
        if (!conn.ctx) {
            tts_conn_close(&conn);
            return -201;
        }
        SSL_CTX_set_default_verify_paths(conn.ctx);
        SSL_CTX_set_verify(conn.ctx, SSL_VERIFY_PEER, NULL);
        conn.ssl = SSL_new(conn.ctx);
        if (!conn.ssl) {
            tts_conn_close(&conn);
            return -201;
        }
        SSL_ctrl(conn.ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, (void*) host);
        X509_VERIFY_PARAM* param = SSL_get0_param(conn.ssl);
        if (!param || !X509_VERIFY_PARAM_set1_host(param, host, 0)) {
            tts_conn_close(&conn);
            return -201;
        }
        SSL_set_fd(conn.ssl, conn.fd);
        if (SSL_connect(conn.ssl) != 1 || SSL_get_verify_result(conn.ssl) != 0) {
            tts_conn_close(&conn);
            return -201;
        }
    }
    char head[1024];
    int headLen = snprintf(head,
                           sizeof head,
                           "POST %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Authorization: Bearer %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: %zu\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           path,
                           host,
                           apiKey,
                           bodyLen);
    if (headLen <= 0 || tts_send_all(&conn, head, (size_t) headLen) ||
        tts_send_all(&conn, body, bodyLen)) {
        tts_conn_close(&conn);
        return -204;
    }
    /* response: headers, then raw wav body */
    unsigned char* headerBuf = malloc(TTS_HTTP_HEADER_MAX + 1);
    if (!headerBuf) {
        tts_conn_close(&conn);
        return -200;
    }
    size_t got = 0;
    int headerDone = 0;
    while (got < TTS_HTTP_HEADER_MAX) {
        ssize_t r = tts_io_read(&conn, headerBuf + got, TTS_HTTP_HEADER_MAX - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t) r;
        headerBuf[got] = 0;
        if (memmem(headerBuf, got, "\r\n\r\n", 4)) {
            headerDone = 1;
            break;
        }
    }
    int rc = -203;
    if (headerDone) {
        int status = (int) strtol((char*) headerBuf + 9, NULL, 10);
        long contentLength = -1;
        int chunked = 0;
        char* sep = memmem(headerBuf, got, "\r\n\r\n", 4);
        size_t bodyHave = got - (size_t) ((char*) sep + 4 - (char*) headerBuf);
        for (char* line = (char*) headerBuf; line < (char*) sep;) {
            char* eol = memchr(line, '\r', (size_t) ((char*) sep - line));
            if (!eol)
                break;
            if (eol - line > 15 && !strncasecmp(line, "Content-Length:", 15))
                contentLength = strtol(line + 15, NULL, 10);
            else if (eol - line > 18 && !strncasecmp(line, "Transfer-Encoding:", 18) &&
                     strcasestr(line, "chunked"))
                chunked = 1;
            line = eol + 2;
        }
        if (status < 200 || status >= 300) {
            rc = -206;
        } else if (chunked) {
            /* streaming TTS responses arrive chunked: read all wire data, then
             * decode the chunk framing in place */
            unsigned char* bodyStart = (unsigned char*) sep + 4;
            size_t wireCap = bodyHave + TTS_WAV_MAX / 2;
            unsigned char* wire = malloc(wireCap + 1);
            if (wire) {
                memcpy(wire, bodyStart, bodyHave);
                size_t wireLen = bodyHave;
                while (wireLen < wireCap) {
                    ssize_t r = tts_io_read(&conn, wire + wireLen, wireCap - wireLen);
                    if (r < 0 && errno == EINTR)
                        continue;
                    if (r <= 0)
                        break;
                    wireLen += (size_t) r;
                }
                unsigned char* w = wire;
                unsigned char* wEnd = wire + wireLen;
                size_t outLen = 0;
                while (outLen + 1 < respCap) {
                    unsigned char* eol = memchr(w, '\r', (size_t) (wEnd - w));
                    if (!eol || eol + 1 >= wEnd)
                        break;
                    long chunkLen = strtol((char*) w, NULL, 16);
                    if (chunkLen <= 0)
                        break;
                    if (wEnd - (eol + 2) < chunkLen)
                        break;
                    if (outLen + (size_t) chunkLen >= respCap)
                        break;
                    memcpy(resp + outLen, eol + 2, (size_t) chunkLen);
                    outLen += (size_t) chunkLen;
                    w = eol + 2 + chunkLen;
                    if (wEnd - w >= 2)
                        w += 2;
                }
                if (outLen) {
                    *respLen = outLen;
                    rc = 0;
                } else {
                    rc = -205;
                }
                free(wire);
            } else {
                rc = -200;
            }
        } else {
            size_t total = contentLength >= 0 ? (size_t) contentLength : bodyHave;
            if (total == 0 || total >= respCap) {
                rc = -206;
            } else {
                unsigned char* bodyStart = (unsigned char*) sep + 4;
                memcpy(resp, bodyStart, bodyHave);
                size_t bodyLen = bodyHave;
                while (bodyLen < total) {
                    ssize_t r = tts_io_read(&conn, resp + bodyLen, total - bodyLen);
                    if (r < 0 && errno == EINTR)
                        continue;
                    if (r <= 0)
                        break;
                    bodyLen += (size_t) r;
                }
                if (bodyLen >= total) {
                    *respLen = bodyLen;
                    rc = 0;
                } else {
                    rc = -205;
                }
            }
        }
    }
    free(headerBuf);
    tts_conn_close(&conn);
    return rc;
}

int TtsSfSynthesize(const char* text, unsigned char** framesOut, size_t* framesLen) {
    *framesOut = NULL;
    *framesLen = 0;
    if (!text || !*text || !TtsSfEnabled())
        return -1;
#ifdef LITECRAB_HAVE_OPUS
    char request[8192];
    size_t requestLen = TtsSfBuildSpeechRequestJson(request, sizeof request,
                                                     g_tts.model, g_tts.voice, text);
    if (!requestLen)
        return -1;
    unsigned char* wav = malloc(TTS_WAV_MAX);
    if (!wav)
        return -1;
    size_t wavLen = 0;
    int httpRc = tts_http_post(g_tts.endpoint, g_tts.apiKey, request, requestLen,
                               g_tts.timeoutMs, wav, TTS_WAV_MAX, &wavLen);
    if (httpRc) {
        LogPrint("[tts] speech http failed rc=%d text=%.60s", httpRc, text);
        free(wav);
        return -1;
    }
    int sampleRate = 0;
    const unsigned char* pcm = NULL;
    size_t pcmLen = 0;
    if (TtsSfParseWavPcm(wav, wavLen, &sampleRate, &pcm, &pcmLen) || !pcmLen) {
        LogPrint("[tts] wav parse failed size=%zu", wavLen);
        free(wav);
        return -1;
    }
    if (sampleRate != g_tts.sampleRate) {
        LogPrint("[tts] wav sample rate %d != configured %d", sampleRate, g_tts.sampleRate);
        free(wav);
        return -1;
    }
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(g_tts.sampleRate, 1, OPUS_APPLICATION_AUDIO, &err);
    if (!enc || err != OPUS_OK) {
        free(wav);
        return -1;
    }
    size_t samplesPerFrame = (size_t) (g_tts.sampleRate / 1000 * g_tts.frameMs);
    size_t frameCount = (pcmLen / 2 + samplesPerFrame - 1) / samplesPerFrame;
    unsigned char* frames = malloc(frameCount * 1300 + 16);
    if (!frames) {
        opus_encoder_destroy(enc);
        free(wav);
        return -1;
    }
    size_t outLen = 0;
    const opus_int16* pcm16 = (const opus_int16*) (const void*) pcm;
    size_t totalSamples = pcmLen / 2;
    for (size_t off = 0; off < totalSamples; off += samplesPerFrame) {
        size_t n = totalSamples - off < samplesPerFrame ? totalSamples - off : samplesPerFrame;
        if (n < samplesPerFrame) {
            /* pad the final partial frame with silence */
            opus_int16* padded = calloc(samplesPerFrame, sizeof(opus_int16));
            if (!padded)
                break;
            memcpy(padded, pcm16 + off, n * sizeof(opus_int16));
            unsigned char packet[1400];
            int written = opus_encode(enc, padded, (opus_int32) samplesPerFrame,
                                      packet, sizeof packet);
            free(padded);
            if (written > 0) {
                frames[outLen++] = (unsigned char) (written >> 8);
                frames[outLen++] = (unsigned char) (written & 0xFF);
                memcpy(frames + outLen, packet, (size_t) written);
                outLen += (size_t) written;
            }
            break;
        }
        unsigned char packet[1400];
        int written = opus_encode(enc, pcm16 + off, (opus_int32) samplesPerFrame,
                                  packet, sizeof packet);
        if (written < 0) {
            LogPrint("[tts] opus_encode failed: %d", written);
            break;
        }
        frames[outLen++] = (unsigned char) (written >> 8);
        frames[outLen++] = (unsigned char) (written & 0xFF);
        memcpy(frames + outLen, packet, (size_t) written);
        outLen += (size_t) written;
    }
    opus_encoder_destroy(enc);
    free(wav);
    if (!outLen)
        return -1;
    *framesOut = frames;
    *framesLen = outLen;
    return 0;
#else
    (void) TTS_HTTP_HEADER_MAX;
    return -1;
#endif
}
