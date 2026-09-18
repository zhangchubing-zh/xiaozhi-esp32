#ifndef LITECRAB_XIAOZHI_WS_H
#define LITECRAB_XIAOZHI_WS_H

#include "litecrab/asr_sf.h"
#include "litecrab/tts_sf.h"

#include <stddef.h>

#define XIAOZHI_WS_MAX_WORKERS 8
#define XIAOZHI_WS_MAX_CONNECTIONS 64

typedef struct {
    int enabled;
    char listenIp[64];
    int listenPort;
    char advertisedIp[64];
    int requireAuth;
    char authToken[128];
    int agentReplyTimeoutMs;
    int maxPayloadBytes;
    int workerThreads;
    int maxConnections;
    int idleTimeoutMs;
    int mcpEnabled;
    XiaozhiAsrConfig asr;
    XiaozhiTtsConfig tts;
} XiaozhiWsConfig;

int XiaozhiWsValidate(const XiaozhiWsConfig* config, char* error, size_t errorSize);
int XiaozhiWsStart(const XiaozhiWsConfig* config);
void XiaozhiWsRequestStop(void);
int XiaozhiWsStop(void);

int XiaozhiWsCheckAuth(const char* authorizationHeader, const char* token);
int XiaozhiBuildCheckpointJson(char* out, size_t outSize, const char* url, const char* token);
int XiaozhiSplitSentences(const char* text,
                          int (*emit)(const char* sentence, void* user),
                          void* user);

#endif
