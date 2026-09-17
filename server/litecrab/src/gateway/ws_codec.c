#include "litecrab/ws_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* OpenSSL EVP ABI for SHA-1 + Base64 (kept hand-declared so minimal WSL images
 * without development headers still link, mirroring src/kernel/llm.c). */
typedef struct evp_md_ctx_st EVP_MD_CTX;
typedef struct evp_md_st EVP_MD;
extern EVP_MD_CTX* EVP_MD_CTX_new(void);
extern void EVP_MD_CTX_free(EVP_MD_CTX*);
extern const EVP_MD* EVP_sha1(void);
extern int EVP_DigestInit_ex(EVP_MD_CTX*, const EVP_MD*, void*);
extern int EVP_DigestUpdate(EVP_MD_CTX*, const void*, size_t);
extern int EVP_DigestFinal_ex(EVP_MD_CTX*, unsigned char*, unsigned int*);
extern int EVP_EncodeBlock(unsigned char*, const unsigned char*, int);

static int header_value_ci(const char* value, const char* needle) {
    if (!value || !needle)
        return 0;
    size_t nz = strlen(needle);
    for (const char* p = value; *p; p++) {
        size_t i = 0;
        while (i < nz && p[i]) {
            char a = p[i], b = needle[i];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b)
                break;
            i++;
        }
        if (i == nz)
            return 1;
    }
    return 0;
}

static void copy_token(const char* src, size_t n, char* out, size_t outSize) {
    if (n >= outSize)
        n = outSize - 1;
    memcpy(out, src, n);
    out[n] = 0;
}

int WsParseHttpHead(const char* buf, size_t len, WsHandshakeRequest* out) {
    if (!buf || !out || len < 4)
        return 0;
    memset(out, 0, sizeof *out);
    out->contentLength = -1;
    const char* end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            end = buf + i + 4;
            break;
        }
    }
    if (!end)
        return len >= WS_HTTP_HEAD_MAX ? -1 : 0;
    const char* lineEnd = memchr(buf, '\r', len);
    if (!lineEnd)
        return -1;
    const char* sp1 = memchr(buf, ' ', (size_t) (lineEnd - buf));
    const char* sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t) (lineEnd - sp1 - 1)) : NULL;
    if (!sp1 || !sp2 || sp1 == buf || sp2 == sp1 + 1)
        return -1;
    copy_token(buf, (size_t) (sp1 - buf), out->method, sizeof out->method);
    copy_token(sp1 + 1, (size_t) (sp2 - sp1 - 1), out->path, sizeof out->path);
    int sawUpgrade = 0, sawConnectionUpgrade = 0;
    const char* p = lineEnd;
    while (p < end) {
        const char* nl = memchr(p, '\r', (size_t) (end - p));
        if (!nl)
            break;
        if (nl - p > 1) {
            const char* colon = memchr(p, ':', (size_t) (nl - p));
            if (colon) {
                const char* v = colon + 1;
                while (v < nl && (*v == ' ' || *v == '\t'))
                    v++;
                size_t vn = (size_t) (nl - v);
                size_t nameLen = (size_t) (colon - p);
                if (nameLen == 7 && !strncasecmp(p, "Upgrade", 7)) {
                    if (header_value_ci(v, "websocket")) {
                        char tmp[32];
                        copy_token(v, vn < sizeof tmp - 1 ? vn : sizeof tmp - 1, tmp, sizeof tmp);
                        sawUpgrade = 1;
                    }
                } else if (nameLen == 10 && !strncasecmp(p, "Connection", 10)) {
                    if (header_value_ci(v, "upgrade"))
                        sawConnectionUpgrade = 1;
                } else if (nameLen == 17 && !strncasecmp(p, "Sec-WebSocket-Key", 17)) {
                    copy_token(v, vn, out->wsKey, sizeof out->wsKey);
                } else if (nameLen == 13 && !strncasecmp(p, "Authorization", 13)) {
                    copy_token(v, vn, out->authorization, sizeof out->authorization);
                } else if (nameLen == 9 && !strncasecmp(p, "Device-Id", 9))
                    copy_token(v, vn, out->deviceId, sizeof out->deviceId);
                else if (nameLen == 9 && !strncasecmp(p, "Client-Id", 9))
                    copy_token(v, vn, out->clientId, sizeof out->clientId);
                else if (nameLen == 14 && !strncasecmp(p, "Content-Length", 14)) {
                    char tmp[24];
                    copy_token(v, vn < sizeof tmp - 1 ? vn : sizeof tmp - 1, tmp, sizeof tmp);
                    out->contentLength = strtol(tmp, NULL, 10);
                }
            }
        }
        p = nl + 2;
    }
    out->headerLength = (int) (end - buf);
    out->isUpgrade = sawUpgrade && sawConnectionUpgrade && out->wsKey[0] &&
                     !strcmp(out->method, "GET");
    return 1;
}

void WsComputeAccept(const char* key, char out[32]) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char concat[128];
    snprintf(concat, sizeof concat, "%s%s", key ? key : "", guid);
    unsigned char digest[20];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        out[0] = 0;
        return;
    }
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(ctx, concat, strlen(concat));
    unsigned int size = 0;
    EVP_DigestFinal_ex(ctx, digest, &size);
    EVP_MD_CTX_free(ctx);
    EVP_EncodeBlock((unsigned char*) out, digest, 20);
    out[28] = 0;
}

enum {
    WS_PARSE_HDR = 0,
    WS_PARSE_LEN16,
    WS_PARSE_LEN64,
    WS_PARSE_MASK,
    WS_PARSE_PAYLOAD,
    WS_PARSE_READY
};

static int valid_opcode(int opcode) {
    return opcode == 0x0 || opcode == 0x1 || opcode == 0x2 || opcode == 0x8 ||
           opcode == 0x9 || opcode == 0xA;
}

static int is_control(int opcode) {
    return opcode == 0x8 || opcode == 0x9 || opcode == 0xA;
}

void WsParserInit(WsParser* p, unsigned char* msgBuf, size_t msgCap) {
    memset(p, 0, sizeof *p);
    p->state = WS_PARSE_HDR;
    p->msgBuf = msgBuf;
    p->msgCap = msgCap;
    p->maxMessageBytes = msgCap;
}

void WsParserConsume(WsParser* p) {
    if (p->state == WS_PARSE_READY) {
        p->state = WS_PARSE_HDR;
        if (p->readyData) {
            p->msgLen = 0;
            p->fragmented = 0;
            p->fragOpcode = 0;
            p->readyData = 0;
        }
    }
}

static size_t fill_from(const unsigned char** d, size_t* avail, unsigned char* dst, int need, int* filled) {
    size_t want = (size_t) (need - *filled);
    size_t give = want < *avail ? want : *avail;
    memcpy(dst + *filled, *d, give);
    *d += give;
    *avail -= give;
    *filled += (int) give;
    return give;
}

int WsParserFeed(WsParser* p, const unsigned char* data, size_t len, size_t* consumed, WsMessage* out) {
    if (!p || !data || !out || p->overflow)
        return -1;
    const unsigned char* d = data;
    size_t avail = len;
    for (;;) {
        if (p->state == WS_PARSE_HDR) {
            while (p->lenfill < 2 && avail)
                fill_from(&d, &avail, p->hdr, 2, &p->lenfill);
            if (p->lenfill < 2) {
                if (consumed)
                    *consumed = len - avail;
                return 0;
            }
            p->lenfill = 0;
            p->curFin = (p->hdr[0] & 0x80) != 0;
            int rsv = p->hdr[0] & 0x70;
            p->curOpcode = p->hdr[0] & 0x0F;
            int masked = (p->hdr[1] & 0x80) != 0;
            unsigned len7 = p->hdr[1] & 0x7F;
            if (rsv || !valid_opcode(p->curOpcode) || !masked)
                return -1;
            if (is_control(p->curOpcode) && (!p->curFin || len7 > WS_CONTROL_MAX))
                return -1;
            if (!is_control(p->curOpcode)) {
                if (p->curOpcode && p->fragmented)
                    return -1;
                if (!p->curOpcode && !p->fragmented)
                    return -1;
            }
            if (len7 <= 125) {
                p->payloadLeft = len7;
                p->lenneed = 0;
                p->lenfill = 0;
                p->maskfill = 0;
                p->state = WS_PARSE_MASK;
            } else if (len7 == 126) {
                p->lenneed = 2;
                p->lenfill = 0;
                p->state = WS_PARSE_LEN16;
            } else {
                p->lenneed = 8;
                p->lenfill = 0;
                p->state = WS_PARSE_LEN64;
            }
            continue;
        }
        if (p->state == WS_PARSE_LEN16 || p->state == WS_PARSE_LEN64) {
            while (p->lenfill < p->lenneed && avail)
                fill_from(&d, &avail, p->lenbuf, p->lenneed, &p->lenfill);
            if (p->lenfill < p->lenneed) {
                if (consumed)
                    *consumed = len - avail;
                return 0;
            }
            p->lenfill = 0;
            uint64_t n = 0;
            for (int i = 0; i < p->lenneed; i++)
                n = (n << 8) | p->lenbuf[i];
            if (p->state == WS_PARSE_LEN64 && (n >> 63))
                return -1;
            if (n > p->maxMessageBytes)
                return -1;
            p->payloadLeft = n;
            p->maskfill = 0;
            p->state = WS_PARSE_MASK;
            continue;
        }
        if (p->state == WS_PARSE_MASK) {
            while (p->maskfill < 4 && avail)
                fill_from(&d, &avail, p->mask, 4, &p->maskfill);
            if (p->maskfill < 4) {
                if (consumed)
                    *consumed = len - avail;
                return 0;
            }
            p->maskfill = 0;
            p->maskIdx = 0;
            p->ctrlLen = 0;
            p->state = WS_PARSE_PAYLOAD;
            continue;
        }
        if (p->state == WS_PARSE_PAYLOAD) {
            if (!p->payloadLeft) {
                if (is_control(p->curOpcode)) {
                    out->type = (WsMsgType) p->curOpcode;
                    out->data = p->ctrl;
                    out->len = p->ctrlLen;
                    p->state = WS_PARSE_READY;
                    p->readyData = 0;
                    if (consumed)
                        *consumed = len - avail;
                    return 1;
                }
                if (p->curOpcode)
                    p->fragOpcode = p->curOpcode;
                if (p->curFin) {
                    out->type = (WsMsgType) p->fragOpcode;
                    out->data = p->msgBuf;
                    out->len = p->msgLen;
                    p->state = WS_PARSE_READY;
                    p->readyData = 1;
                    if (consumed)
                        *consumed = len - avail;
                    return 1;
                }
                p->fragmented = 1;
                p->state = WS_PARSE_HDR;
                continue;
            }
            if (!avail) {
                if (consumed)
                    *consumed = len - avail;
                return 0;
            }
            size_t chunk = p->payloadLeft < avail ? (size_t) p->payloadLeft : avail;
            if (is_control(p->curOpcode)) {
                for (size_t i = 0; i < chunk; i++)
                    p->ctrl[p->ctrlLen++] = d[i] ^ p->mask[(p->maskIdx++) & 3];
            } else {
                if (p->msgLen + chunk > p->msgCap) {
                    p->overflow = 1;
                    return -1;
                }
                for (size_t i = 0; i < chunk; i++)
                    p->msgBuf[p->msgLen++] = d[i] ^ p->mask[(p->maskIdx++) & 3];
            }
            d += chunk;
            avail -= chunk;
            p->payloadLeft -= chunk;
            continue;
        }
        if (p->state == WS_PARSE_READY) {
            if (consumed)
                *consumed = len - avail;
            return 0;
        }
        return -1;
    }
}

int WsEncodeFrame(int opcode, const unsigned char* payload, size_t len, unsigned char* out, size_t cap) {
    if (!out || (!payload && len))
        return -1;
    size_t header = 2;
    if (len <= 125) {
        out[1] = (unsigned char) len;
    } else if (len <= 0xFFFF) {
        out[1] = 126;
        out[2] = (unsigned char) (len >> 8);
        out[3] = (unsigned char) len;
        header = 4;
    } else {
        out[1] = 127;
        uint64_t n = len;
        for (int i = 0; i < 8; i++)
            out[2 + i] = (unsigned char) (n >> (56 - 8 * i));
        header = 10;
    }
    out[0] = (unsigned char) (0x80 | (opcode & 0x0F));
    if (cap < header + len)
        return -1;
    if (len)
        memcpy(out + header, payload, len);
    return (int) (header + len);
}
