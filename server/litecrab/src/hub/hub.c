#include "litecrab/hub.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    LiteMsg q[LITE_BUS_QUEUE_LEN];
    int head, tail, count, ready;
    pthread_mutex_t mutex;
    pthread_cond_t notEmpty, notFull;
} BusQueue;
typedef struct {
    LiteMsg q[LITE_BUS_QUEUE_LEN];
    int head, tail, count, capacity;
} SchedulerLane;
typedef struct {
    SchedulerLane control, alarm, normal;
    int ready, normalsSinceAlarm;
    pthread_mutex_t mutex;
    pthread_cond_t notEmpty, notFull;
} Scheduler;
typedef enum { RESPONSE_EMPTY = 0, RESPONSE_WAITING, RESPONSE_READY } ResponseState;
typedef struct {
    ResponseState state;
    char requestId[LITE_MAX_REQUEST_ID_LEN];
    LiteMsg response;
} ResponseSlot;

#define REQUEST_REGISTRY_LEN 32
static Scheduler scheduler;
static BusQueue outq;
static ResponseSlot responses[REQUEST_REGISTRY_LEN];
static pthread_mutex_t responseMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t responseChanged;
static int responseReady;
static pthread_mutex_t initMu = PTHREAD_MUTEX_INITIALIZER, seqMu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long seq;

int64_t LiteMonotonicMs(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static void cond_init_monotonic(pthread_cond_t* condition) {
    pthread_condattr_t attributes;
    pthread_condattr_init(&attributes);
    pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    pthread_cond_init(condition, &attributes);
    pthread_condattr_destroy(&attributes);
}
static void queue_init(BusQueue* q) {
    if (q->ready)
        return;
    memset(q, 0, sizeof *q);
    pthread_mutex_init(&q->mutex, NULL);
    cond_init_monotonic(&q->notEmpty);
    cond_init_monotonic(&q->notFull);
    q->ready = 1;
}
static void scheduler_init(Scheduler* s) {
    if (s->ready)
        return;
    memset(s, 0, sizeof *s);
    s->control.capacity = LITE_CONTROL_QUEUE_LEN;
    s->alarm.capacity = LITE_ALARM_QUEUE_LEN;
    s->normal.capacity = LITE_NORMAL_QUEUE_LEN;
    pthread_mutex_init(&s->mutex, NULL);
    cond_init_monotonic(&s->notEmpty);
    cond_init_monotonic(&s->notFull);
    s->ready = 1;
}
int MessageBusInit(void) {
    pthread_mutex_lock(&initMu);
    scheduler_init(&scheduler);
    queue_init(&outq);
    if (!responseReady) {
        memset(responses, 0, sizeof responses);
        cond_init_monotonic(&responseChanged);
        responseReady = 1;
    }
    pthread_mutex_unlock(&initMu);
    return 0;
}
static void lane_clear(SchedulerLane* lane) {
    for (int i = 0; i < lane->count; i++)
        free(lane->q[(lane->head + i) % LITE_BUS_QUEUE_LEN].content);
    lane->head = lane->tail = lane->count = 0;
}
static void scheduler_destroy(Scheduler* s) {
    if (!s->ready)
        return;
    pthread_mutex_lock(&s->mutex);
    lane_clear(&s->control);
    lane_clear(&s->alarm);
    lane_clear(&s->normal);
    pthread_mutex_unlock(&s->mutex);
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->notEmpty);
    pthread_cond_destroy(&s->notFull);
    s->ready = 0;
}
static void queue_destroy(BusQueue* q) {
    if (!q->ready)
        return;
    pthread_mutex_lock(&q->mutex);
    for (int i = 0; i < q->count; i++)
        free(q->q[(q->head + i) % LITE_BUS_QUEUE_LEN].content);
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->notEmpty);
    pthread_cond_destroy(&q->notFull);
    q->ready = 0;
}
void MessageBusDestroy(void) {
    pthread_mutex_lock(&initMu);
    scheduler_destroy(&scheduler);
    queue_destroy(&outq);
    if (responseReady) {
        pthread_mutex_lock(&responseMu);
        for (int i = 0; i < REQUEST_REGISTRY_LEN; i++) {
            free(responses[i].response.content);
            memset(&responses[i], 0, sizeof responses[i]);
        }
        pthread_mutex_unlock(&responseMu);
        pthread_cond_destroy(&responseChanged);
        responseReady = 0;
    }
    pthread_mutex_unlock(&initMu);
}
static int push(BusQueue* q, const LiteMsg* m, int block) {
    if (!q->ready)
        MessageBusInit();
    pthread_mutex_lock(&q->mutex);
    while (q->count == LITE_BUS_QUEUE_LEN) {
        if (!block) {
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }
        pthread_cond_wait(&q->notFull, &q->mutex);
    }
    q->q[q->tail] = *m;
    q->tail = (q->tail + 1) % LITE_BUS_QUEUE_LEN;
    q->count++;
    pthread_cond_broadcast(&q->notEmpty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}
static int pop(BusQueue* q, LiteMsg* m) {
    if (!q->ready)
        MessageBusInit();
    pthread_mutex_lock(&q->mutex);
    while (!q->count)
        pthread_cond_wait(&q->notEmpty, &q->mutex);
    *m = q->q[q->head];
    memset(&q->q[q->head], 0, sizeof *m);
    q->head = (q->head + 1) % LITE_BUS_QUEUE_LEN;
    q->count--;
    pthread_cond_signal(&q->notFull);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}
static SchedulerLane* select_lane(Scheduler* s, const LiteMsg* m) {
    if (m->type != LITE_MSG_CHAT)
        return &s->control;
    if (m->priority >= LITE_PRIORITY_HIGH && !strncmp(m->source, "alarm:", 6))
        return &s->alarm;
    return &s->normal;
}
static int scheduler_push(const LiteMsg* m, int block) {
    if (!scheduler.ready)
        MessageBusInit();
    pthread_mutex_lock(&scheduler.mutex);
    SchedulerLane* lane = select_lane(&scheduler, m);
    while (lane->count == lane->capacity) {
        if (!block) {
            pthread_mutex_unlock(&scheduler.mutex);
            return -1;
        }
        pthread_cond_wait(&scheduler.notFull, &scheduler.mutex);
    }
    lane->q[lane->tail] = *m;
    lane->tail = (lane->tail + 1) % LITE_BUS_QUEUE_LEN;
    lane->count++;
    pthread_cond_broadcast(&scheduler.notEmpty);
    pthread_mutex_unlock(&scheduler.mutex);
    return 0;
}
static void lane_pop(SchedulerLane* lane, LiteMsg* m) {
    *m = lane->q[lane->head];
    memset(&lane->q[lane->head], 0, sizeof *m);
    lane->head = (lane->head + 1) % LITE_BUS_QUEUE_LEN;
    lane->count--;
}
static int scheduler_pop(LiteMsg* m) {
    if (!scheduler.ready)
        MessageBusInit();
    pthread_mutex_lock(&scheduler.mutex);
    while (!scheduler.control.count && !scheduler.alarm.count && !scheduler.normal.count)
        pthread_cond_wait(&scheduler.notEmpty, &scheduler.mutex);
    if (scheduler.control.count) {
        lane_pop(&scheduler.control, m);
    } else if (scheduler.alarm.count &&
               (!scheduler.normal.count || scheduler.normalsSinceAlarm >= 2)) {
        lane_pop(&scheduler.alarm, m);
        scheduler.normalsSinceAlarm = 0;
    } else {
        lane_pop(&scheduler.normal, m);
        scheduler.normalsSinceAlarm++;
    }
    pthread_cond_broadcast(&scheduler.notFull);
    pthread_mutex_unlock(&scheduler.mutex);
    return 0;
}
int MessageBusPushInbound(const LiteMsg* m) { return m ? scheduler_push(m, 1) : -1; }
int MessageBusTryPushInbound(const LiteMsg* m) { return m ? scheduler_push(m, 0) : -1; }
int MessageBusPopInbound(LiteMsg* m) { return m ? scheduler_pop(m) : -1; }
int MessageBusPushOutbound(const LiteMsg* m) { return m ? push(&outq, m, 1) : -1; }
int MessageBusPopOutbound(LiteMsg* m) { return m ? pop(&outq, m) : -1; }
static int find(BusQueue* q, const char* id) {
    for (int i = 0; i < q->count; i++) {
        int p = (q->head + i) % LITE_BUS_QUEUE_LEN;
        if (!strcmp(q->q[p].requestId, id))
            return i;
    }
    return -1;
}
static void remove_at(BusQueue* q, int logical, LiteMsg* m) {
    int p = (q->head + logical) % LITE_BUS_QUEUE_LEN;
    *m = q->q[p];
    for (int i = logical; i < q->count - 1; i++) {
        int a = (q->head + i) % LITE_BUS_QUEUE_LEN, b = (q->head + i + 1) % LITE_BUS_QUEUE_LEN;
        q->q[a] = q->q[b];
    }
    q->tail = (q->tail - 1 + LITE_BUS_QUEUE_LEN) % LITE_BUS_QUEUE_LEN;
    memset(&q->q[q->tail], 0, sizeof *m);
    q->count--;
    pthread_cond_signal(&q->notFull);
}
int MessageBusPopOutboundByRequestId(const char* id, LiteMsg* m) {
    if (!id || !m)
        return -1;
    pthread_mutex_lock(&outq.mutex);
    int p = find(&outq, id);
    if (p >= 0)
        remove_at(&outq, p, m);
    pthread_mutex_unlock(&outq.mutex);
    return p >= 0 ? 0 : -1;
}
static void absolute_deadline(struct timespec* t, int ms) {
    clock_gettime(CLOCK_MONOTONIC, t);
    t->tv_sec += ms / 1000;
    t->tv_nsec += (long) (ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}
int MessageBusTimedPopOutboundByRequestId(const char* id, LiteMsg* m, int ms) {
    if (!id || !m || ms < 0)
        return -1;
    if (!outq.ready)
        MessageBusInit();
    struct timespec until;
    absolute_deadline(&until, ms);
    pthread_mutex_lock(&outq.mutex);
    for (;;) {
        int p = find(&outq, id);
        if (p >= 0) {
            remove_at(&outq, p, m);
            pthread_mutex_unlock(&outq.mutex);
            return 0;
        }
        int rc = pthread_cond_timedwait(&outq.notEmpty, &outq.mutex, &until);
        if (rc) {
            pthread_mutex_unlock(&outq.mutex);
            return -1;
        }
    }
}
void LiteMsgClear(LiteMsg* m) {
    if (m) {
        free(m->content);
        memset(m, 0, sizeof *m);
    }
}
static int response_find(const char* id) {
    for (int i = 0; i < REQUEST_REGISTRY_LEN; i++)
        if (responses[i].state != RESPONSE_EMPTY && !strcmp(responses[i].requestId, id))
            return i;
    return -1;
}
static int response_register(const char* id) {
    pthread_mutex_lock(&responseMu);
    int slot = -1;
    for (int i = 0; i < REQUEST_REGISTRY_LEN; i++)
        if (responses[i].state == RESPONSE_EMPTY) {
            slot = i;
            break;
        }
    if (slot >= 0) {
        responses[slot].state = RESPONSE_WAITING;
        snprintf(responses[slot].requestId, sizeof responses[slot].requestId, "%s", id);
    }
    pthread_mutex_unlock(&responseMu);
    return slot >= 0 ? 0 : -1;
}
int RequestComplete(LiteMsg* response) {
    if (!response || !response->requestId[0])
        return -1;
    if (!responseReady)
        MessageBusInit();
    pthread_mutex_lock(&responseMu);
    int slot = response_find(response->requestId);
    if (slot < 0 || responses[slot].state != RESPONSE_WAITING) {
        pthread_mutex_unlock(&responseMu);
        return -1;
    }
    responses[slot].response = *response;
    response->content = NULL;
    responses[slot].state = RESPONSE_READY;
    pthread_cond_broadcast(&responseChanged);
    pthread_mutex_unlock(&responseMu);
    return 0;
}
int RequestCancel(const char* requestId) {
    if (!requestId || !responseReady)
        return -1;
    pthread_mutex_lock(&responseMu);
    int slot = response_find(requestId);
    if (slot >= 0) {
        free(responses[slot].response.content);
        memset(&responses[slot], 0, sizeof responses[slot]);
        pthread_cond_broadcast(&responseChanged);
    }
    pthread_mutex_unlock(&responseMu);
    return slot >= 0 ? 0 : -1;
}
int RequestCancelAll(void) {
    if (!responseReady)
        return 0;
    int cancelled = 0;
    pthread_mutex_lock(&responseMu);
    for (int i = 0; i < REQUEST_REGISTRY_LEN; i++) {
        if (responses[i].state == RESPONSE_EMPTY)
            continue;
        free(responses[i].response.content);
        memset(&responses[i], 0, sizeof responses[i]);
        cancelled++;
    }
    if (cancelled)
        pthread_cond_broadcast(&responseChanged);
    pthread_mutex_unlock(&responseMu);
    return cancelled;
}
int RequestShouldStop(const LiteMsg* request) {
    if (!request)
        return 1;
    if (request->deadlineMs > 0 && LiteMonotonicMs() >= request->deadlineMs)
        return 1;
    if (request->replyMode != LITE_REPLY_SYNC)
        return 0;
    if (!responseReady)
        return 1;
    pthread_mutex_lock(&responseMu);
    int slot = response_find(request->requestId);
    pthread_mutex_unlock(&responseMu);
    return slot < 0;
}
int IngressInit(void) { return MessageBusInit(); }
static void cp(char* d, size_t z, const char* s) { snprintf(d, z, "%s", s ? s : ""); }
int IngressSubmit(
    const char* source, const char* raw, int len, const IngressOptions* o, IngressResult* r) {
    if (r)
        memset(r, 0, sizeof *r);
    if (!r)
        return -1;
    if (!raw || len <= 0 || len > INGRESS_MAX_REQ_BYTES) {
        r->status = INGRESS_REJECTED;
        return -1;
    }
    LiteMsg m = {0};
    m.type = o ? o->type : LITE_MSG_CHAT;
    m.priority = o ? o->priority : LITE_PRIORITY_NORMAL;
    m.replyMode = o ? o->replyMode : LITE_REPLY_ACK_ONLY;
    m.deadlineMs = o && o->deadlineMs > 0
                       ? o->deadlineMs
                       : LiteMonotonicMs() + (m.priority >= LITE_PRIORITY_HIGH ? 180000 : 120000);
    const char* src = source ? source : o && o->source ? o->source : "unknown";
    cp(m.source, sizeof m.source, src);
    const char* colon = strchr(src, ':');
    size_t n = colon ? (size_t) (colon - src) : strlen(src);
    if (n >= sizeof m.channel)
        n = sizeof m.channel - 1;
    memcpy(m.channel, src, n);
    m.channel[n] = 0;
    cp(m.userId, sizeof m.userId, o && o->userId ? o->userId : "anonymous");
    cp(m.sessionId, sizeof m.sessionId, o && o->sessionId ? o->sessionId : "default");
    cp(m.chatId, sizeof m.chatId, m.sessionId);
    cp(m.replyToRunId, sizeof m.replyToRunId, o && o->replyToRunId ? o->replyToRunId : "");
    cp(m.replyToInterruptId,
       sizeof m.replyToInterruptId,
       o && o->replyToInterruptId ? o->replyToInterruptId : "");
    cp(m.correlationToken,
       sizeof m.correlationToken,
       o && o->correlationToken ? o->correlationToken : "");
    pthread_mutex_lock(&seqMu);
    snprintf(m.requestId, sizeof m.requestId, "req-%lu", ++seq);
    pthread_mutex_unlock(&seqMu);
    m.content = malloc((size_t) len + 1);
    if (!m.content) {
        r->status = INGRESS_BUSY;
        return -1;
    }
    memcpy(m.content, raw, (size_t) len);
    m.content[len] = 0;
    int registered = m.replyMode == LITE_REPLY_SYNC && !response_register(m.requestId);
    if ((m.replyMode == LITE_REPLY_SYNC && !registered) || MessageBusTryPushInbound(&m)) {
        if (registered)
            RequestCancel(m.requestId);
        free(m.content);
        r->status = INGRESS_BUSY;
        return -1;
    }
    r->status = INGRESS_ACCEPTED;
    cp(r->requestId, sizeof r->requestId, m.requestId);
    return 0;
}
int DispatchResponseEx(const char* id,
                       LiteReplyMode mode,
                       int timeout,
                       char* resp,
                       int z,
                       LiteCompletionStatus* completionStatus) {
    if (!resp || z <= 0 || !id || timeout < 0)
        return -1;
    if (mode == LITE_REPLY_ACK_ONLY) {
        resp[0] = 0;
        if (completionStatus)
            *completionStatus = LITE_COMPLETION_HANDLED;
        return 0;
    }
    struct timespec until;
    absolute_deadline(&until, timeout);
    pthread_mutex_lock(&responseMu);
    int slot = response_find(id);
    while (slot >= 0 && responses[slot].state == RESPONSE_WAITING) {
        int rc = pthread_cond_timedwait(&responseChanged, &responseMu, &until);
        if (rc)
            break;
        slot = response_find(id);
    }
    if (slot < 0 || responses[slot].state != RESPONSE_READY) {
        if (slot >= 0) {
            free(responses[slot].response.content);
            memset(&responses[slot], 0, sizeof responses[slot]);
        }
        pthread_mutex_unlock(&responseMu);
        snprintf(resp, (size_t) z, "ERROR: response timeout");
        if (completionStatus)
            *completionStatus = LITE_COMPLETION_RETRYABLE;
        return -1;
    }
    LiteMsg message = responses[slot].response;
    memset(&responses[slot], 0, sizeof responses[slot]);
    pthread_mutex_unlock(&responseMu);
    snprintf(resp, (size_t) z, "%s", message.content ? message.content : "");
    if (completionStatus)
        *completionStatus = message.completionStatus;
    LiteMsgClear(&message);
    return 0;
}
int DispatchResponse(const char* id, LiteReplyMode mode, int timeout, char* resp, int z) {
    return DispatchResponseEx(id, mode, timeout, resp, z, NULL);
}
