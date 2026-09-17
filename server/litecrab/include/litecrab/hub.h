#ifndef LITECRAB_HUB_H
#define LITECRAB_HUB_H
#include <stddef.h>
#include <stdint.h>
#define LITE_BUS_QUEUE_LEN 16
#define LITE_CONTROL_QUEUE_LEN 4
#define LITE_ALARM_QUEUE_LEN 8
#define LITE_NORMAL_QUEUE_LEN 8
#define LITE_OUTBOUND_WORKERS 1
#define LITE_AGENT_MAX_TOOL_ITER 16
#define LITE_MAX_CHANNEL_LEN 32
#define LITE_MAX_CHAT_ID_LEN 64
#define LITE_MAX_REQUEST_ID_LEN 64
#define LITE_MAX_SOURCE_LEN 32
#define LITE_MAX_USER_ID_LEN 64
#define LITE_MAX_SESSION_ID_LEN 64
#define INGRESS_ACCEPTED 0
#define INGRESS_REJECTED 1
#define INGRESS_BUSY 2
#define INGRESS_MAX_REQ_BYTES (16 * 1024)
typedef enum {
    LITE_MSG_CHAT,
    LITE_MSG_EXCEPTION,
    LITE_MSG_STATUS_QUERY,
    LITE_MSG_ACTION_REQUEST,
    LITE_MSG_SESSION_OPEN,
    LITE_MSG_SESSION_CLOSE
} LiteMsgType;
typedef enum {
    LITE_PRIORITY_LOW = 0,
    LITE_PRIORITY_NORMAL = 1,
    LITE_PRIORITY_HIGH = 2,
    LITE_PRIORITY_CRITICAL = 3
} LitePriority;
typedef enum { LITE_REPLY_SYNC, LITE_REPLY_ACK_ONLY } LiteReplyMode;
typedef enum {
    LITE_COMPLETION_HANDLED = 0,
    LITE_COMPLETION_RETRYABLE = 1,
    LITE_COMPLETION_PERMANENT = 2,
    LITE_COMPLETION_CANCELLED = 3
} LiteCompletionStatus;
typedef struct {
    LiteMsgType type;
    LitePriority priority;
    LiteReplyMode replyMode;
    char source[32], channel[32], chatId[64], userId[64], sessionId[64], requestId[64];
    char replyToRunId[64], replyToInterruptId[64], correlationToken[64];
    int64_t deadlineMs;
    LiteCompletionStatus completionStatus;
    char* content;
} LiteMsg;
typedef struct {
    const char *source, *userId, *sessionId;
    const char *replyToRunId, *replyToInterruptId, *correlationToken;
    LiteMsgType type;
    LitePriority priority;
    LiteReplyMode replyMode;
    int64_t deadlineMs;
} IngressOptions;
typedef struct {
    int status;
    char requestId[64];
} IngressResult;
int MessageBusInit(void);
void MessageBusDestroy(void);
int MessageBusPushInbound(const LiteMsg*);
int MessageBusTryPushInbound(const LiteMsg*);
int MessageBusPopInbound(LiteMsg*);
int MessageBusPushOutbound(const LiteMsg*);
int MessageBusPopOutbound(LiteMsg*);
int MessageBusPopOutboundByRequestId(const char*, LiteMsg*);
int MessageBusTimedPopOutboundByRequestId(const char*, LiteMsg*, int);
int IngressInit(void);
int IngressSubmit(const char*, const char*, int, const IngressOptions*, IngressResult*);
int DispatchResponse(const char*, LiteReplyMode, int, char*, int);
int DispatchResponseEx(
    const char*, LiteReplyMode, int, char*, int, LiteCompletionStatus* completionStatus);
int RequestComplete(LiteMsg* response);
int RequestCancel(const char* requestId);
int RequestCancelAll(void);
int RequestShouldStop(const LiteMsg* request);
int64_t LiteMonotonicMs(void);
void LiteMsgClear(LiteMsg*);
#endif
