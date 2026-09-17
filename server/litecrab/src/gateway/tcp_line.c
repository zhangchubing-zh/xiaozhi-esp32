#include "litecrab/gateway.h"
#include "litecrab/hub.h"
#include "litecrab/kernel.h"
#include "litecrab/json.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
    RequestServerConfig cfg;
    RequestHandlerFn handler;
    void* user;
} Client;
#define SERVER_MAX_WORKERS 8
#define SERVER_MAX_CONNECTIONS 64
typedef struct ServerState ServerState;
typedef struct {
    ServerState* state;
    int index;
} WorkerArg;
struct ServerState {
    pthread_mutex_t mu;
    pthread_cond_t available;
    Client* pending[SERVER_MAX_CONNECTIONS];
    int head, tail, count, total, stopping;
    int workerCount;
    int activeFd[SERVER_MAX_WORKERS];
    pthread_t workers[SERVER_MAX_WORKERS];
    WorkerArg workerArgs[SERVER_MAX_WORKERS];
};
static pthread_mutex_t idMu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long idSeq;
static volatile sig_atomic_t serverStopRequested;
void RequestServerRequestStop(void) { serverStopRequested = 1; }
static void session_id(char* out, size_t z) {
    pthread_mutex_lock(&idMu);
    unsigned long n = ++idSeq;
    pthread_mutex_unlock(&idMu);
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    snprintf(out,
             z,
             "tmp:sess-%lu-%08lx",
             (unsigned long) t.tv_sec,
             (n ^ (unsigned long) t.tv_nsec) & 0xffffffffUL);
}
static int send_all(int fd, const char* d, size_t n) {
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
static int recv_line(int fd, char* out, size_t z) {
    size_t n = 0;
    while (n + 1 < z) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        if (c == '\n') {
            if (n && out[n - 1] == '\r')
                n--;
            out[n] = 0;
            return (int) n;
        }
        out[n++] = c;
    }
    out[z - 1] = 0;
    return -2;
}
static void normalize_line_response(char* text) {
    if (!text)
        return;
    size_t read = 0, write = 0;
    int lineBreak = 0;
    while (text[read]) {
        unsigned char ch = (unsigned char) text[read++];
        if (ch == '\r' || ch == '\n') {
            lineBreak = 1;
            continue;
        }
        if (lineBreak && write && text[write - 1] != ' ' && ch != ' ' && ch != '\t')
            text[write++] = ' ';
        lineBreak = 0;
        text[write++] = (char) ch;
    }
    while (write && (text[write - 1] == ' ' || text[write - 1] == '\t'))
        write--;
    text[write] = 0;
}
static void emit_session(LiteMsgType type, const char* session) {
    IngressOptions o = {.source = "network:tcp",
                        .userId = "anonymous",
                        .sessionId = session,
                        .type = type,
                        .priority = LITE_PRIORITY_NORMAL,
                        .replyMode = LITE_REPLY_ACK_ONLY};
    IngressResult r;
    IngressSubmit("network:tcp",
                  type == LITE_MSG_SESSION_OPEN ? "open" : "close",
                  type == LITE_MSG_SESSION_OPEN ? 4 : 5,
                  &o,
                  &r);
}
static void* client_main(void* arg) {
    Client* c = arg;
    struct timeval rv = {c->cfg.recvTimeoutMs / 1000, (c->cfg.recvTimeoutMs % 1000) * 1000},
                   sv = {c->cfg.sendTimeoutMs / 1000, (c->cfg.sendTimeoutMs % 1000) * 1000};
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &rv, sizeof rv);
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &sv, sizeof sv);
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    ConnectionContext ctx = {
        .connFd = c->fd, .requestTimeoutMs = c->cfg.recvTimeoutMs, .user = c->user};
    session_id(ctx.sessionId, sizeof ctx.sessionId);
    emit_session(LITE_MSG_SESSION_OPEN, ctx.sessionId);
    size_t rz = (size_t) c->cfg.maxReqBytes + 1, sz = (size_t) c->cfg.maxReqBytes * 4 + 2;
    char *req = malloc(rz), *resp = malloc(sz);
    if (req && resp)
        for (;;) {
            int n = recv_line(c->fd, req, rz);
            if (n < 0)
                break;
            int rc = c->handler(req, resp, (int) sz, &ctx);
            if (rc < 0)
                break;
            char responseFormat[32] = "";
            JsonExtractStringField(req, "responseFormat", responseFormat, sizeof responseFormat);
            if (!strcmp(responseFormat, "json")) {
                /* JSON escaping preserves Markdown while keeping one wire line. */
                size_t wireSize = strlen(resp) * 6 + 64;
                char* wire = malloc(wireSize);
                if (!wire)
                    break;
                LjBuf b;
                LjBufInit(&b, wire, wireSize);
                LjAppend(&b, "{\"responseFormat\":\"json\",\"content\":");
                LjAppendJsonString(&b, resp);
                LjAppend(&b, "}\n");
                int failed = b.failed || send_all(c->fd, wire, b.len);
                free(wire);
                if (failed)
                    break;
                continue;
            }
            normalize_line_response(resp);
            size_t len = strlen(resp);
            if (len + 1 >= sz)
                break;
            if (!len || resp[len - 1] != '\n')
                resp[len++] = '\n';
            if (send_all(c->fd, resp, len))
                break;
        }
    free(req);
    free(resp);
    emit_session(LITE_MSG_SESSION_CLOSE, ctx.sessionId);
    close(c->fd);
    free(c);
    return NULL;
}
static void* connection_worker(void* arg) {
    WorkerArg* worker = arg;
    ServerState* state = worker->state;
    for (;;) {
        pthread_mutex_lock(&state->mu);
        while (!state->count && !state->stopping)
            pthread_cond_wait(&state->available, &state->mu);
        if (state->stopping) {
            pthread_mutex_unlock(&state->mu);
            break;
        }
        Client* client = state->pending[state->head];
        state->pending[state->head] = NULL;
        state->head = (state->head + 1) % SERVER_MAX_CONNECTIONS;
        state->count--;
        state->activeFd[worker->index] = client->fd;
        pthread_mutex_unlock(&state->mu);

        client_main(client);

        pthread_mutex_lock(&state->mu);
        state->activeFd[worker->index] = -1;
        if (state->total > 0)
            state->total--;
        pthread_mutex_unlock(&state->mu);
    }
    return NULL;
}
int RequestServerValidateBindPolicy(const RequestServerConfig* config,
                                   char* error,
                                   size_t errorSize) {
    if (error && errorSize)
        error[0] = 0;
    if (!config)
        return -1;
    struct in_addr addr;
    if (inet_pton(AF_INET, config->listenIp, &addr) != 1) {
        snprintf(error,
                 errorSize,
                 "invalid IPv4 listen address: %s",
                 config->listenIp[0] ? config->listenIp : "(empty)");
        return -1;
    }
    if (config->allowUnauthenticatedRemote != 0 && config->allowUnauthenticatedRemote != 1) {
        snprintf(error, errorSize, "invalid allowUnauthenticatedRemote value");
        return -1;
    }
    unsigned int host = ntohl(addr.s_addr);
    int is_loopback = (host & 0xff000000U) == 0x7f000000U;
    if (!is_loopback && !config->allowUnauthenticatedRemote) {
        snprintf(error,
                 errorSize,
                 "unauthenticated remote TCP requires allow_unauthenticated_remote=true");
        return -1;
    }
    return 0;
}
int StartRequestServer(const RequestServerConfig* in, RequestHandlerFn h, void* u) {
    if (!h)
        return -1;
    RequestServerConfig cfg = {0};
    if (in)
        cfg = *in;
    if (!cfg.listenIp[0])
        snprintf(cfg.listenIp, sizeof cfg.listenIp, "127.0.0.1");
    if (cfg.allowUnauthenticatedRemote != 0 && cfg.allowUnauthenticatedRemote != 1)
        cfg.allowUnauthenticatedRemote = 0;
    if (cfg.listenPort <= 0)
        cfg.listenPort = 10003;
    if (cfg.backlog <= 0)
        cfg.backlog = 32;
    if (cfg.recvTimeoutMs <= 0)
        cfg.recvTimeoutMs = 30000;
    if (cfg.sendTimeoutMs <= 0)
        cfg.sendTimeoutMs = 30000;
    if (cfg.maxReqBytes <= 0)
        cfg.maxReqBytes = 16384;
    if (cfg.workerThreads <= 0)
        cfg.workerThreads = 4;
    if (cfg.maxConnections <= 0)
        cfg.maxConnections = 16;
    if (cfg.workerThreads > SERVER_MAX_WORKERS || cfg.maxConnections > SERVER_MAX_CONNECTIONS ||
        cfg.maxConnections < cfg.workerThreads)
        return -1;
    char bindError[256];
    if (RequestServerValidateBindPolicy(&cfg, bindError, sizeof bindError)) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t) cfg.listenPort)};
    if (inet_pton(AF_INET, cfg.listenIp, &a.sin_addr) != 1 ||
        bind(fd, (struct sockaddr*) &a, sizeof a) || listen(fd, cfg.backlog)) {
        close(fd);
        return -1;
    }
    ServerState state;
    memset(&state, 0, sizeof state);
    pthread_mutex_init(&state.mu, NULL);
    pthread_cond_init(&state.available, NULL);
    state.workerCount = cfg.workerThreads;
    for (int i = 0; i < SERVER_MAX_WORKERS; i++)
        state.activeFd[i] = -1;
    int started = 0;
    for (; started < state.workerCount; started++) {
        state.workerArgs[started] = (WorkerArg){.state = &state, .index = started};
        if (pthread_create(
                &state.workers[started], NULL, connection_worker, &state.workerArgs[started]))
            break;
    }
    if (started != state.workerCount) {
        pthread_mutex_lock(&state.mu);
        state.stopping = 1;
        pthread_cond_broadcast(&state.available);
        pthread_mutex_unlock(&state.mu);
        for (int i = 0; i < started; i++)
            pthread_join(state.workers[i], NULL);
        pthread_cond_destroy(&state.available);
        pthread_mutex_destroy(&state.mu);
        close(fd);
        return -1;
    }
    while (!serverStopRequested) {
        struct pollfd ready = {.fd = fd, .events = POLLIN};
        int waitResult = poll(&ready, 1, 100);
        if (waitResult < 0 && errno == EINTR)
            continue;
        if (waitResult < 0) {
            close(fd);
            return -1;
        }
        if (!waitResult)
            continue;
        int conn = accept(fd, NULL, NULL);
        if (conn < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        Client* c = malloc(sizeof *c);
        if (!c) {
            close(conn);
            continue;
        }
        *c = (Client){conn, cfg, h, u};
        pthread_mutex_lock(&state.mu);
        if (state.total >= cfg.maxConnections || state.count >= SERVER_MAX_CONNECTIONS) {
            pthread_mutex_unlock(&state.mu);
            static const char busy[] = "ERROR: server busy\n";
            send(conn, busy, sizeof busy - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
            close(conn);
            free(c);
            continue;
        }
        state.pending[state.tail] = c;
        state.tail = (state.tail + 1) % SERVER_MAX_CONNECTIONS;
        state.count++;
        state.total++;
        pthread_cond_signal(&state.available);
        pthread_mutex_unlock(&state.mu);
    }
    close(fd);
    /* Wake handlers blocked on synchronous responses before joining their
     * connection workers. The signal handler itself remains lock-free. */
    RequestCancelAll();
    pthread_mutex_lock(&state.mu);
    state.stopping = 1;
    for (int i = 0; i < state.workerCount; i++)
        if (state.activeFd[i] >= 0)
            shutdown(state.activeFd[i], SHUT_RDWR);
    while (state.count) {
        Client* client = state.pending[state.head];
        state.pending[state.head] = NULL;
        state.head = (state.head + 1) % SERVER_MAX_CONNECTIONS;
        state.count--;
        if (state.total > 0)
            state.total--;
        close(client->fd);
        free(client);
    }
    pthread_cond_broadcast(&state.available);
    pthread_mutex_unlock(&state.mu);
    for (int i = 0; i < state.workerCount; i++)
        pthread_join(state.workers[i], NULL);
    pthread_cond_destroy(&state.available);
    pthread_mutex_destroy(&state.mu);
    return 0;
}
int LiteCrabHandleNetworkRequest(const char* line, char* resp, int z, ConnectionContext* ctx) {
    if (!line || !resp || z <= 0 || !ctx)
        return -1;
    char user[64], requestedSession[64] = "";
    snprintf(user, sizeof user, "%s", ctx->userId[0] ? ctx->userId : "anonymous");
    char content[INGRESS_MAX_REQ_BYTES + 1];
    char replyToRunId[64] = "", replyToInterruptId[64] = "", correlationToken[64] = "";
    while (isspace((unsigned char) *line))
        line++;
    if (*line == '{') {
        if (!JsonExtractStringField(line, "content", content, sizeof content)) {
            snprintf(resp,
                     (size_t) z,
                     "ERROR: invalid JSON request; string field 'content' is required");
            return 0;
        }
        JsonExtractStringField(line, "userId", user, sizeof user);
        snprintf(ctx->userId, sizeof ctx->userId, "%s", user);
        JsonExtractStringField(line, "sessionId", requestedSession, sizeof requestedSession);
        if (requestedSession[0] &&
            (AgentSessionIdValidate(requestedSession) || !strncmp(requestedSession, "tmp:", 4))) {
            snprintf(resp, (size_t) z, "ERROR: invalid sessionId");
            return 0;
        }
        JsonExtractStringField(line, "replyToRunId", replyToRunId, sizeof replyToRunId);
        JsonExtractStringField(
            line, "replyToInterruptId", replyToInterruptId, sizeof replyToInterruptId);
        JsonExtractStringField(line, "correlationToken", correlationToken, sizeof correlationToken);
    } else {
        size_t n = strlen(line);
        while (n && isspace((unsigned char) line[n - 1]))
            n--;
        if (!n || n >= sizeof content) {
            snprintf(resp, (size_t) z, "ERROR: plain-text request is empty or too large");
            return 0;
        }
        memcpy(content, line, n);
        content[n] = 0;
    }
    IngressOptions o = {.source = "network:tcp",
                        .userId = user,
                        .sessionId = requestedSession[0] ? requestedSession : ctx->sessionId,
                        .type = LITE_MSG_CHAT,
                        .priority = LITE_PRIORITY_NORMAL,
                        .replyMode = LITE_REPLY_SYNC};
    o.deadlineMs = LiteMonotonicMs() +
                   (ctx->requestTimeoutMs > 0 ? ctx->requestTimeoutMs : 30000);
    o.replyToRunId = replyToRunId;
    o.replyToInterruptId = replyToInterruptId;
    o.correlationToken = correlationToken;
    IngressResult r;
    if (IngressSubmit("network:tcp", content, (int) strlen(content), &o, &r)) {
        snprintf(resp, (size_t) z, "ERROR: request rejected");
        return 0;
    }
    DispatchResponse(r.requestId,
                     LITE_REPLY_SYNC,
                     ctx->requestTimeoutMs > 0 ? ctx->requestTimeoutMs : 30000,
                     resp,
                     z);
    return 0;
}
