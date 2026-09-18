#ifndef LITECRAB_WS_CODEC_H
#define LITECRAB_WS_CODEC_H

#include <stddef.h>
#include <stdint.h>

#define WS_HTTP_HEAD_MAX (16 * 1024)
#define WS_CONTROL_MAX 125

typedef enum {
    WS_MSG_TEXT = 0x1,
    WS_MSG_BINARY = 0x2,
    WS_MSG_CLOSE = 0x8,
    WS_MSG_PING = 0x9,
    WS_MSG_PONG = 0xA
} WsMsgType;

typedef struct {
    char method[8];
    char path[256];
    char wsKey[64];
    char authorization[512];
    char deviceId[64];
    char clientId[64];
    long contentLength;
    int isUpgrade;
    int headerLength;
} WsHandshakeRequest;

int WsParseHttpHead(const char* buf, size_t len, WsHandshakeRequest* out);
void WsComputeAccept(const char* key, char out[32]);

typedef struct {
    size_t maxMessageBytes;
    unsigned char* msgBuf;
    size_t msgLen, msgCap;
    int overflow;
    int state;
    int fragmented;
    int fragOpcode;
    unsigned char hdr[2];
    unsigned char lenbuf[8];
    int lenfill, lenneed;
    int maskfill;
    unsigned char mask[4];
    uint64_t payloadLeft;
    uint64_t maskIdx;
    unsigned char curOpcode;
    int curFin;
    int readyData;
    unsigned char ctrl[WS_CONTROL_MAX];
    size_t ctrlLen;
} WsParser;

typedef struct {
    WsMsgType type;
    const unsigned char* data;
    size_t len;
} WsMessage;

void WsParserInit(WsParser* p, unsigned char* msgBuf, size_t msgCap);
int WsParserFeed(WsParser* p,
                 const unsigned char* data,
                 size_t len,
                 size_t* consumed,
                 WsMessage* out);
void WsParserConsume(WsParser* p);
int WsEncodeFrame(int opcode, const unsigned char* payload, size_t len, unsigned char* out, size_t cap);

#endif
