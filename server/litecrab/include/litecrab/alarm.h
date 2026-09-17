#ifndef LITECRAB_ALARM_H
#define LITECRAB_ALARM_H
#include <stddef.h>
#include <stdint.h>
#define ALARM_MAX_TEXT_LEN 65536
#define ALARM_MESSAGE_MAX 4096
#define ALARM_QUEUE_CAPACITY 64
#define ALARM_SEEN_CAPACITY 1024
#define ALARM_FORWARD_RETRIES 3 /* initial attempt + three retries */
typedef enum {
    ALARM_POLL_IDLE = 0,
    ALARM_POLL_NOTIFY,
    ALARM_POLL_RESPONSE_END,
    ALARM_POLL_AUTH_EXPIRED,
    ALARM_POLL_NETWORK_ERROR
} AlarmPollResult;
typedef enum { ALARM_MODE_POLLING = 0, ALARM_MODE_SUBSCRIPTION = 1 } AlarmMode;
typedef struct {
    char raw[ALARM_MAX_TEXT_LEN];    /* transient ASP response; never persisted or forwarded */
    char message[ALARM_MESSAGE_MAX]; /* extracted fields forwarded to Agent */
    char key[65];                    /* SHA256 identity persisted for delivery deduplication */
    int active;                      /* zero: explicit empty snapshot; resets consecutive dedup */
} AlarmNotify;
typedef struct AspAlarmClient AspAlarmClient;
typedef struct {
    int (*begin)(AspAlarmClient*, uint64_t);
    int (*poll)(AspAlarmClient*, uint32_t, AlarmNotify*);
    int (*cancel)(AspAlarmClient*);
    void (*destroy)(AspAlarmClient*);
} AspAlarmClientVTable;
struct AspAlarmClient {
    const AspAlarmClientVTable* vtable;
    void* impl;
};
typedef struct {
    char baseUrl[512], user[128], password[256], langlist[16];
    char caFile[1024];
    char spoolDirectory[1024];
    int ioTickMs, pollIntervalMs, longPollDeadlineMs, retryDelayMs, insecureTls;
    AlarmMode mode;
    AspAlarmClient* client; /* required by core; production factory is explicit */
} AlarmConfig;
typedef struct AlarmService AlarmService;
int AlarmNotifyParse(const char* raw, AlarmNotify* out);
int AlarmServiceInit(AlarmService** out, const AlarmConfig* cfg);
int AlarmServiceStart(AlarmService* svc);
/* Quiesce stops reception and new one-way submissions. */
int AlarmServiceQuiesce(AlarmService* svc, uint32_t timeoutMs);
int AlarmServiceStop(AlarmService* svc, uint32_t timeoutMs);
/* Does not free a service with unjoined threads. Caller owns injected client. */
void AlarmServiceDestroy(AlarmService* svc);
AspAlarmClient* AspAlarmClientCurlCreate(const AlarmConfig* cfg);
#endif
