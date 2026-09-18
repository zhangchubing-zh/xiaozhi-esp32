#ifndef LITECRAB_GATEWAY_H
#define LITECRAB_GATEWAY_H
#include <stddef.h>
typedef struct {
    int connFd;
    int requestTimeoutMs;
    char sessionId[64];
    char userId[64];
    void* user;
} ConnectionContext;
typedef int (*RequestHandlerFn)(const char*, char*, int, ConnectionContext*);
typedef struct {
    char listenIp[64];
    int listenPort, backlog, recvTimeoutMs, sendTimeoutMs, maxReqBytes;
    int workerThreads, maxConnections;
    int allowUnauthenticatedRemote;
} RequestServerConfig;
int RequestServerValidateBindPolicy(const RequestServerConfig* config,
                                    char* error,
                                    size_t errorSize);
int StartRequestServer(const RequestServerConfig*, RequestHandlerFn, void*);
void RequestServerRequestStop(void);
int LiteCrabHandleNetworkRequest(const char*, char*, int, ConnectionContext*);
#endif
