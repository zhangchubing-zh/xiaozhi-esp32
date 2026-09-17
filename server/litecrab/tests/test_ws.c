#define _GNU_SOURCE
#include "litecrab/asr_sf.h"
#include "litecrab/json.h"
#include "litecrab/ws_codec.h"
#include "litecrab/xiaozhi_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;

#define CHECK(x)                                                  \
    do {                                                          \
        checks++;                                                 \
        if (!(x)) {                                               \
            failures++;                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);   \
        }                                                         \
    } while (0)

static size_t make_frame(unsigned char* out,
                         size_t cap,
                         int fin,
                         int opcode,
                         const unsigned char* payload,
                         size_t len,
                         int masked) {
    size_t h = 2;
    out[0] = (unsigned char) ((fin ? 0x80 : 0) | (opcode & 0xF));
    if (len <= 125) {
        out[1] = (unsigned char) ((masked ? 0x80 : 0) | len);
    } else if (len <= 0xFFFF) {
        out[1] = (unsigned char) ((masked ? 0x80 : 0) | 126);
        out[2] = (unsigned char) (len >> 8);
        out[3] = (unsigned char) len;
        h = 4;
    } else {
        out[1] = (unsigned char) ((masked ? 0x80 : 0) | 127);
        unsigned long long n = len;
        for (int i = 0; i < 8; i++)
            out[2 + i] = (unsigned char) (n >> (56 - 8 * i));
        h = 10;
    }
    if (masked) {
        unsigned char key[4] = {0x37, 0xfa, 0x21, 0x3d};
        memcpy(out + h, key, 4);
        h += 4;
        for (size_t i = 0; i < len; i++)
            out[h + i] = payload[i] ^ key[i & 3];
        return h + len;
    }
    if (h + len > cap)
        return 0;
    memcpy(out + h, payload, len);
    return h + len;
}

static unsigned char msgBuf[512 * 1024];

static void test_accept(void) {
    char accept[32];
    WsComputeAccept("dGhlIHNhbXBsZSBub25jZQ==", accept);
    CHECK(!strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    WsComputeAccept("Y2hhcmFjdGVyc2V0cw==", accept);
    CHECK(strlen(accept) == 28);
}

static void test_http_head(void) {
    const char* req =
        "GET /xiaozhi/v1/ HTTP/1.1\r\n"
        "Host: 192.168.1.10:8000\r\n"
        "Authorization: Bearer tok-123\r\n"
        "Device-Id: AA:BB:CC:DD:EE:FF\r\n"
        "Client-Id: uuid-1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    WsHandshakeRequest parsed;
    CHECK(WsParseHttpHead(req, strlen(req), &parsed) == 1);
    CHECK(parsed.isUpgrade == 1);
    CHECK(!strcmp(parsed.method, "GET"));
    CHECK(!strcmp(parsed.path, "/xiaozhi/v1/"));
    CHECK(!strcmp(parsed.wsKey, "dGhlIHNhbXBsZSBub25jZQ=="));
    CHECK(!strcmp(parsed.authorization, "Bearer tok-123"));
    CHECK(!strcmp(parsed.deviceId, "AA:BB:CC:DD:EE:FF"));
    CHECK(!strcmp(parsed.clientId, "uuid-1"));
    CHECK(parsed.headerLength == (int) strlen(req));
    const char* post =
        "POST /xiaozhi/ota/ HTTP/1.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 1234\r\n"
        "\r\n{}";
    CHECK(WsParseHttpHead(post, strlen(post), &parsed) == 1);
    CHECK(parsed.isUpgrade == 0);
    CHECK(!strcmp(parsed.method, "POST"));
    CHECK(parsed.contentLength == 1234);
    CHECK(WsParseHttpHead("GET /x HTTP/1.1\r\nHost: a\r\n", 22, &parsed) == 0);
    const char* noConn =
        "GET /xiaozhi/v1/ HTTP/1.1\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Key: abc\r\n"
        "\r\n";
    CHECK(WsParseHttpHead(noConn, strlen(noConn), &parsed) == 1);
    CHECK(parsed.isUpgrade == 0);
}

static void test_encode(void) {
    unsigned char out[16];
    CHECK(WsEncodeFrame(WS_MSG_CLOSE, NULL, 0, out, sizeof out) == 2);
    CHECK(out[0] == 0x88 && out[1] == 0x00);
    CHECK(WsEncodeFrame(WS_MSG_PING, (const unsigned char*) "hi", 2, out, sizeof out) == 4);
    CHECK(out[0] == 0x89 && out[1] == 0x02 && out[2] == 'h' && out[3] == 'i');
    unsigned char big[300];
    memset(big, 0xAB, sizeof big);
    CHECK(WsEncodeFrame(WS_MSG_TEXT, big, 126, out, sizeof out) == -1);
    unsigned char out2[512];
    CHECK(WsEncodeFrame(WS_MSG_TEXT, big, 126, out2, sizeof out2) == 4 + 126);
    CHECK(out2[1] == 126 && out2[2] == 0 && out2[3] == 126);
    CHECK(WsEncodeFrame(WS_MSG_TEXT, big, 200, out2, sizeof out2) == 4 + 200);
    CHECK(out2[1] == 126);
}

static void feed_expect(WsParser* p,
                        const unsigned char* frame,
                        size_t frameLen,
                        WsMsgType wantType,
                        const unsigned char* wantData,
                        size_t wantLen) {
    size_t consumed = 0;
    WsMessage msg;
    int rc = WsParserFeed(p, frame, frameLen, &consumed, &msg);
    CHECK(rc == 1);
    if (rc == 1) {
        CHECK(msg.type == wantType);
        CHECK(msg.len == wantLen);
        CHECK(!memcmp(msg.data, wantData, wantLen));
        CHECK(consumed == frameLen);
    }
    WsParserConsume(p);
}

static void test_roundtrip(void) {
    unsigned char frame[1024];
    WsParser p;
    size_t n = make_frame(frame, sizeof frame, 1, WS_MSG_TEXT, (const unsigned char*) "hello", 5, 1);
    WsParserInit(&p, msgBuf, sizeof msgBuf);
    feed_expect(&p, frame, n, WS_MSG_TEXT, (const unsigned char*) "hello", 5);
    unsigned char blob[200];
    for (int i = 0; i < 200; i++)
        blob[i] = (unsigned char) i;
    n = make_frame(frame, sizeof frame, 1, WS_MSG_BINARY, blob, 200, 1);
    feed_expect(&p, frame, n, WS_MSG_BINARY, blob, 200);
    n = make_frame(frame, sizeof frame, 1, WS_MSG_PING, (const unsigned char*) "xy", 2, 1);
    feed_expect(&p, frame, n, WS_MSG_PING, (const unsigned char*) "xy", 2);
    /* byte-by-byte feed */
    n = make_frame(frame, sizeof frame, 1, WS_MSG_TEXT, (const unsigned char*) "split", 5, 1);
    WsParserInit(&p, msgBuf, sizeof msgBuf);
    int got = 0;
    for (size_t i = 0; i < n && !got; i++) {
        size_t consumed = 0;
        WsMessage msg;
        int rc = WsParserFeed(&p, frame + i, 1, &consumed, &msg);
        if (rc == 1) {
            got = 1;
            CHECK(msg.type == WS_MSG_TEXT && msg.len == 5 && !memcmp(msg.data, "split", 5));
            WsParserConsume(&p);
        }
    }
    CHECK(got);
}

static void test_fragmentation(void) {
    unsigned char frame[1024];
    WsParser p;
    WsParserInit(&p, msgBuf, sizeof msgBuf);
    size_t n = make_frame(frame, sizeof frame, 0, WS_MSG_TEXT, (const unsigned char*) "foo", 3, 1);
    size_t consumed = 0;
    WsMessage msg;
    CHECK(WsParserFeed(&p, frame, n, &consumed, &msg) == 0);
    n = make_frame(frame, sizeof frame, 0, 0x0, (const unsigned char*) "bar", 3, 1);
    CHECK(WsParserFeed(&p, frame, n, &consumed, &msg) == 0);
    n = make_frame(frame, sizeof frame, 1, WS_MSG_PING, (const unsigned char*) "!", 1, 1);
    CHECK(WsParserFeed(&p, frame, n, &consumed, &msg) == 1);
    CHECK(msg.type == WS_MSG_PING && msg.len == 1 && msg.data[0] == '!');
    WsParserConsume(&p);
    n = make_frame(frame, sizeof frame, 1, 0x0, (const unsigned char*) "baz", 3, 1);
    CHECK(WsParserFeed(&p, frame, n, &consumed, &msg) == 1);
    CHECK(msg.type == WS_MSG_TEXT && msg.len == 9);
    CHECK(!memcmp(msg.data, "foobarbaz", 9));
    WsParserConsume(&p);
}

static void test_protocol_errors(void) {
    unsigned char frame[1024];
    WsParser p;
    size_t consumed = 0;
    WsMessage msg;
    WsParserInit(&p, msgBuf, sizeof msgBuf);
    size_t n = make_frame(frame, sizeof frame, 1, WS_MSG_TEXT, (const unsigned char*) "x", 1, 0);
    CHECK(WsParserFeed(&p, frame, n, &consumed, &msg) == -1);
    frame[0] = 0xC1; /* RSV bits set, masked text */
    n = make_frame(frame, sizeof frame, 1, WS_MSG_TEXT, (const unsigned char*) "x", 1, 1);
    frame[0] = 0xC1;
    CHECK(WsParserFeed(&p, frame, n, &consumed, &msg) == -1);
    n = make_frame(frame, sizeof frame, 1, 0x3, (const unsigned char*) "x", 1, 1);
    CHECK(WsParserFeed(&p, frame, n, &consumed, &msg) == -1);
    n = make_frame(frame, sizeof frame, 1, 0x0, (const unsigned char*) "x", 1, 1);
    CHECK(WsParserFeed(&p, frame, n, &consumed, &msg) == -1);
    n = make_frame(frame, sizeof frame, 0, WS_MSG_TEXT, (const unsigned char*) "x", 1, 1);
    CHECK(WsParserFeed(&p, frame, n, &consumed, &msg) == 0);
    n = make_frame(frame, sizeof frame, 1, WS_MSG_TEXT, (const unsigned char*) "y", 1, 1);
    CHECK(WsParserFeed(&p, frame, n, &consumed, &msg) == -1);
    WsParserInit(&p, msgBuf, sizeof msgBuf);
    frame[0] = 0x89; /* ping with 126-byte payload: invalid */
    frame[1] = 0xFE;
    frame[2] = 0x00;
    frame[3] = 0x7E;
    CHECK(WsParserFeed(&p, frame, 4, &consumed, &msg) == -1);
    WsParserInit(&p, msgBuf, 10);
    n = make_frame(frame, sizeof frame, 1, WS_MSG_TEXT, (const unsigned char*) "0123456789ABCDEF", 16, 1);
    CHECK(WsParserFeed(&p, frame, n, &consumed, &msg) == -1);
}

static void test_lengths(void) {
    unsigned char frame[70 * 1024];
    unsigned char payload[70 * 1024];
    for (size_t i = 0; i < sizeof payload; i++)
        payload[i] = (unsigned char) (i * 7);
    size_t lengths[] = {125, 126, 65535, 65536};
    for (size_t t = 0; t < sizeof lengths / sizeof lengths[0]; t++) {
        size_t len = lengths[t];
        WsParser p;
        WsParserInit(&p, msgBuf, sizeof msgBuf);
        size_t n = make_frame(frame, sizeof frame, 1, WS_MSG_BINARY, payload, len, 1);
        CHECK(n > 0);
        size_t consumed = 0;
        WsMessage msg;
        int rc = WsParserFeed(&p, frame, n, &consumed, &msg);
        CHECK(rc == 1);
        if (rc == 1) {
            CHECK(msg.len == len);
            CHECK(!memcmp(msg.data, payload, len));
        }
        WsParserConsume(&p);
    }
}

static int collect_count;
static char collected[32][600];

static int collect(const char* sentence, void* user) {
    (void) user;
    if (collect_count < 32)
        snprintf(collected[collect_count], sizeof collected[collect_count], "%s", sentence);
    collect_count++;
    return 0;
}

static void test_sentences(void) {
    collect_count = 0;
    CHECK(XiaozhiSplitSentences("你好。世界！test? yes; no\nend", collect, NULL) == 6);
    CHECK(collect_count == 6);
    CHECK(!strcmp(collected[0], "你好"));
    CHECK(!strcmp(collected[1], "世界"));
    CHECK(!strcmp(collected[2], "test"));
    CHECK(!strcmp(collected[3], "yes"));
    CHECK(!strcmp(collected[4], "no"));
    CHECK(!strcmp(collected[5], "end"));
    collect_count = 0;
    CHECK(XiaozhiSplitSentences("", collect, NULL) == 0);
    CHECK(XiaozhiSplitSentences("  。 ！！ ", collect, NULL) == 0);
    collect_count = 0;
    char long1[600];
    for (int i = 0; i < 200; i++)
        long1[i] = 'a' + (i % 26);
    long1[200] = 0;
    int n = XiaozhiSplitSentences(long1, collect, NULL);
    CHECK(n >= 1);
    CHECK(strlen(collected[0]) <= 120);
    char utf8Long[600];
    int off = 0;
    for (int i = 0; i < 80; i++)
        off += snprintf(utf8Long + off, sizeof utf8Long - off, "汉字");
    collect_count = 0;
    n = XiaozhiSplitSentences(utf8Long, collect, NULL);
    CHECK(n >= 1);
    for (int i = 0; i < collect_count && i < 32; i++) {
        size_t len = strlen(collected[i]);
        CHECK(len % 3 == 0); /* no split UTF-8 char: each 汉字 is 3 bytes */
    }
}

static void test_wav_header(void) {
    unsigned char hdr[44];
    CHECK(AsrSfBuildWavHeader(hdr, 32000, 16000, 1, 16) == 0);
    CHECK(!memcmp(hdr, "RIFF", 4));
    CHECK(!memcmp(hdr + 8, "WAVE", 4));
    CHECK(!memcmp(hdr + 12, "fmt ", 4));
    CHECK(!memcmp(hdr + 36, "data", 4));
    unsigned int riff = 0, rate = 0, dataSize = 0;
    memcpy(&riff, hdr + 4, 4);
    memcpy(&rate, hdr + 24, 4);
    memcpy(&dataSize, hdr + 40, 4);
    CHECK(riff == 32000 + 36);
    CHECK(rate == 16000);
    CHECK(dataSize == 32000);
    unsigned short bits = 0;
    memcpy(&bits, hdr + 34, 2);
    CHECK(bits == 16);
}

static void test_multipart(void) {
    unsigned char wav[64];
    for (int i = 0; i < 64; i++)
        wav[i] = (unsigned char) i;
    char body[2048];
    size_t n = AsrSfBuildMultipartBody(body, sizeof body, "BND", "model-x", wav, 64);
    CHECK(n > 0);
    body[n] = 0;
    CHECK(strstr(body, "--BND\r\n"));
    CHECK(strstr(body, "name=\"model\""));
    CHECK(strstr(body, "model-x"));
    CHECK(strstr(body, "filename=\"audio.wav\""));
    CHECK(strstr(body, "audio/wav"));
    CHECK(memmem(body, n, "\r\n--BND--\r\n", strlen("\r\n--BND--\r\n")) != NULL);
    CHECK(memmem(body, n, wav, 64) != NULL);
}

static void test_asr_parse(void) {
    char text[256];
    CHECK(AsrSfParseTranscriptionResponse("{\"text\":\"你好世界\"}", text, sizeof text) == 0);
    CHECK(!strcmp(text, "你好世界"));
    CHECK(AsrSfParseTranscriptionResponse("{\"text\":\"\"}", text, sizeof text) == 0);
    CHECK(!text[0]);
    CHECK(AsrSfParseTranscriptionResponse("{\"usage\":{}}", text, sizeof text) == -1);
    CHECK(AsrSfParseTranscriptionResponse("not json", text, sizeof text) == -1);
    CHECK(AsrSfParseTranscriptionResponse("{\"text\":\"\\u00e9\\u4f60\"}", text, sizeof text) == 0);
    CHECK(!strcmp(text, "é你"));
}

static void test_checkpoint_json(void) {
    char json[512];
    CHECK(XiaozhiBuildCheckpointJson(json, sizeof json, "ws://1.2.3.4:8000/xiaozhi/v1/", "tok") == 0);
    CHECK(strstr(json, "\"websocket\""));
    CHECK(strstr(json, "ws://1.2.3.4:8000/xiaozhi/v1/"));
    CHECK(strstr(json, "\"token\":\"tok\""));
    CHECK(strstr(json, "\"version\":1"));
    CHECK(strstr(json, "\"server_time\""));
    CHECK(!strstr(json, "mqtt"));
    CHECK(LjValidate(json, LJ_OBJECT) == 0);
}

static void test_auth(void) {
    CHECK(XiaozhiWsCheckAuth("Bearer tok", "tok") == 1);
    CHECK(XiaozhiWsCheckAuth("tok", "tok") == 1);
    CHECK(XiaozhiWsCheckAuth("Bearer bad", "tok") == 0);
    CHECK(XiaozhiWsCheckAuth("", "tok") == 0);
    CHECK(XiaozhiWsCheckAuth(NULL, "tok") == 0);
    CHECK(XiaozhiWsCheckAuth("anything", "") == 1);
    CHECK(XiaozhiWsCheckAuth(NULL, "") == 1);
}

static void test_validate(void) {
    XiaozhiWsConfig c;
    char error[256];
    memset(&c, 0, sizeof c);
    snprintf(c.listenIp, sizeof c.listenIp, "0.0.0.0");
    c.listenPort = 8000;
    c.agentReplyTimeoutMs = 120000;
    c.maxPayloadBytes = 262144;
    c.workerThreads = 8;
    c.maxConnections = 64;
    c.idleTimeoutMs = 300000;
    snprintf(c.asr.provider, sizeof c.asr.provider, "siliconflow");
    c.asr.timeoutMs = 15000;
    c.asr.maxAudioSeconds = 60;
    CHECK(XiaozhiWsValidate(&c, error, sizeof error) == 0);
    c.listenPort = 0;
    CHECK(XiaozhiWsValidate(&c, error, sizeof error) == -1);
    c.listenPort = 8000;
    c.requireAuth = 1;
    CHECK(XiaozhiWsValidate(&c, error, sizeof error) == -1);
    snprintf(c.authToken, sizeof c.authToken, "tok");
    CHECK(XiaozhiWsValidate(&c, error, sizeof error) == 0);
    snprintf(c.asr.provider, sizeof c.asr.provider, "bogus");
    CHECK(XiaozhiWsValidate(&c, error, sizeof error) == -1);
    snprintf(c.asr.provider, sizeof c.asr.provider, "stub");
    CHECK(XiaozhiWsValidate(&c, error, sizeof error) == 0);
    c.workerThreads = 9;
    CHECK(XiaozhiWsValidate(&c, error, sizeof error) == -1);
}

static void test_stub_provider(void) {
    XiaozhiAsrConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.provider, sizeof cfg.provider, "stub");
    snprintf(cfg.stubInputText, sizeof cfg.stubInputText, "预设指令内容");
    CHECK(AsrSfInit(&cfg) == 0);
    XiaozhiAsrProvider* p = AsrSfCreateProvider();
    CHECK(p != NULL);
    if (p) {
        char text[256];
        p->begin(p, "sess");
        p->feed_audio(p, (const unsigned char*) "fake", 4);
        p->feed_audio(p, (const unsigned char*) "fake2", 5);
        CHECK(p->finish(p, "sess", text, sizeof text) == 0);
        CHECK(!strcmp(text, "预设指令内容"));
        p->abort(p);
        AsrSfDestroyProvider(p);
    }
}

int main(void) {
    test_accept();
    test_http_head();
    test_encode();
    test_roundtrip();
    test_fragmentation();
    test_protocol_errors();
    test_lengths();
    test_sentences();
    test_wav_header();
    test_multipart();
    test_asr_parse();
    test_checkpoint_json();
    test_auth();
    test_validate();
    test_stub_provider();
    printf("ws tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
