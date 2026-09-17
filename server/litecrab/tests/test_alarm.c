#include "../src/alarm/monitor.h"
#include "litecrab/hub.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int checks;
static int failures;

#define CHECK(expression)                                                 \
    do {                                                                  \
        checks++;                                                         \
        if (!(expression)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

typedef struct {
    pthread_mutex_t mu;
    AlarmNotify notification;
    int ready;
    int cancelled;
    int delay;
} Mock;

static int begin(AspAlarmClient* client, uint64_t generation) {
    (void) generation;
    Mock* mock = client->impl;
    pthread_mutex_lock(&mock->mu);
    mock->cancelled = 0;
    pthread_mutex_unlock(&mock->mu);
    return 0;
}

static int poll_mock(AspAlarmClient* client, uint32_t milliseconds, AlarmNotify* out) {
    Mock* mock = client->impl;
    pthread_mutex_lock(&mock->mu);
    int delay = mock->delay;
    pthread_mutex_unlock(&mock->mu);
    usleep((delay ? delay : (int) milliseconds) * 1000);
    pthread_mutex_lock(&mock->mu);
    int ready = mock->ready && !mock->cancelled;
    if (ready) {
        *out = mock->notification;
        mock->ready = 0;
    }
    pthread_mutex_unlock(&mock->mu);
    return ready ? ALARM_POLL_NOTIFY : ALARM_POLL_IDLE;
}

static int cancel(AspAlarmClient* client) {
    Mock* mock = client->impl;
    pthread_mutex_lock(&mock->mu);
    mock->cancelled = 1;
    pthread_mutex_unlock(&mock->mu);
    return 0;
}

static void destroy(AspAlarmClient* client) {
    (void) client;
}

static const AspAlarmClientVTable vtable = {begin, poll_mock, cancel, destroy};

static void publish(Mock* mock, int sequence, const char* time) {
    char raw[4096];
    snprintf(raw,
             sizeof raw,
             "{\"errcode\":0,\"almlist\":[{\"seqno\":%d,\"almid\":1154,"
             "\"almname\":\"通信异常\",\"equipid\":1,\"equiptypeid\":33028,"
             "\"equipname\":\"Logger\",\"level\":2,\"reason\":671,"
             "\"position\":1,\"localtime\":\"%s\","
             "\"faultDesc\":\"储能通信异常\",\"description\":\"不应传给模型\","
             "\"subReasonList\":[{\"subReason\":\"通信线缆异常\","
             "\"subRepair\":\"检查通信线缆\"}]}]}",
             sequence,
             time);
    pthread_mutex_lock(&mock->mu);
    CHECK(!AlarmNotifyParse(raw, &mock->notification));
    mock->ready = 1;
    pthread_mutex_unlock(&mock->mu);
    usleep(120000);
}

static AlarmService* start_service_at(AlarmConfig* config,
                                      Mock* mock,
                                      AspAlarmClient* client,
                                      int retryDelay,
                                      const char* spoolDirectory) {
    memset(config, 0, sizeof *config);
    strcpy(config->baseUrl, "http://mock");
    config->mode = ALARM_MODE_SUBSCRIPTION;
    config->ioTickMs = 10;
    config->retryDelayMs = retryDelay;
    if (spoolDirectory)
        snprintf(config->spoolDirectory, sizeof config->spoolDirectory, "%s", spoolDirectory);
    client->vtable = &vtable;
    client->impl = mock;
    config->client = client;
    AlarmService* service = NULL;
    CHECK(!AlarmServiceInit(&service, config));
    CHECK(!AlarmServiceStart(service));
    return service;
}

static AlarmService*
start_service(AlarmConfig* config, Mock* mock, AspAlarmClient* client, int retryDelay) {
    return start_service_at(config, mock, client, retryDelay, NULL);
}

static void complete_request_status(LiteMsg* request, LiteCompletionStatus status) {
    LiteMsg response = {0};
    snprintf(response.requestId, sizeof response.requestId, "%s", request->requestId);
    response.replyMode = request->replyMode;
    response.completionStatus = status;
    response.content = strdup("agent processing completed");
    CHECK(!RequestComplete(&response));
    LiteMsgClear(request);
    usleep(30000);
}

static void complete_request(LiteMsg* request) {
    complete_request_status(request, LITE_COMPLETION_HANDLED);
}

static void parse_tests(void) {
    AlarmNotify first;
    AlarmNotify second;
    CHECK(!AlarmNotifyParse("{\"CriticalNum\":1,\"MajorNum\":0}", &first));
    CHECK(!AlarmNotifyParse("{ \"MajorNum\":0, \"CriticalNum\":1 }", &second));
    CHECK(!strcmp(first.key, second.key));
    CHECK(!AlarmNotifyParse("{\"errcode\":0,\"almlist\":[]}", &first));
    CHECK(!first.active);
    CHECK(AlarmNotifyParse("ERR", &first));
    CHECK(AlarmNotifyParse("<html>login</html>", &first));
    CHECK(AlarmNotifyParse("{\"CriticalNum\":1,\"CriticalNum\":2}", &first));

    const char* row = "{\"seqno\":24,\"almid\":1154,\"almname\":\"通信异常\",\"equipid\":1,"
                      "\"equiptypeid\":33028,\"equipname\":\"Logger\",\"level\":2,"
                      "\"reason\":671,\"position\":1,\"localtime\":\"2026-09-08 10:56:52\","
                      "\"faultDesc\":\"储能通信异常\",\"description\":\"原始长描述\","
                      "\"subReasonList\":[{\"subReason\":\"通信线缆异常\","
                      "\"subRepair\":\"检查通信线缆\"}]}";
    CHECK(!AlarmOccurrenceParse(row, &first));
    CHECK(strstr(first.message, "告警ID ：1154,"));
    CHECK(strstr(first.message, "告警名称 ：通信异常,"));
    CHECK(strstr(first.message, "告警原因 ：储能通信异常,"));
    CHECK(strstr(first.message, "告警序列号 ：24,"));
    CHECK(strstr(first.message, "告警时间 ：2026-09-08 10:56:52,"));
    CHECK(strstr(first.message, "原因说明 ：通信线缆异常,"));
    CHECK(strstr(first.message, "建议处理 ：检查通信线缆,"));
    CHECK(strstr(first.message, "使用 PLC_Diagnosis 完整告警诊断、修复和回归验证"));
    CHECK(!strstr(first.message, "原始长描述"));
    CHECK(AlarmOccurrenceParse("{\"seqno\":24,\"almid\":1154,\"equipid\":1}", &first));

    /* Deterministic malformed-input barrage. The parser may accept or reject
     * each case, but it must remain memory-safe and bounded. */
    uint32_t random = 0x5091u;
    char fuzz[513];
    static const char alphabet[] = "{}[],:\\\"0123456789abcdefghijklmnopqrstuvwxyz<> \n\t";
    for (int round = 0; round < 10000; round++) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        size_t length = random % (sizeof fuzz - 1);
        for (size_t i = 0; i < length; i++) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            fuzz[i] = alphabet[random % (sizeof alphabet - 1)];
        }
        fuzz[length] = 0;
        AlarmNotifyParse(fuzz, &first);
        AlarmOccurrenceParse(fuzz, &second);
    }
    CHECK(1);
}

static void delivery_and_dedup_test(void) {
    AlarmConfig config;
    Mock mock = {.mu = PTHREAD_MUTEX_INITIALIZER};
    AspAlarmClient client;
    MessageBusInit();
    AlarmService* service = start_service(&config, &mock, &client, 50);

    publish(&mock, 1, "2026-09-08 10:00:00");
    LiteMsg message = {0};
    MessageBusPopInbound(&message);
    CHECK(message.replyMode == LITE_REPLY_SYNC);
    CHECK(strstr(message.content, "告警ID ：1154,"));
    CHECK(strstr(message.content, "告警序列号 ：1,"));
    CHECK(!strstr(message.content, "description"));
    complete_request(&message);

    publish(&mock, 1, "2026-09-08 10:00:00");
    LiteMsg marker = {0};
    strcpy(marker.requestId, "marker");
    marker.content = strdup("marker");
    CHECK(!MessageBusTryPushInbound(&marker));
    MessageBusPopInbound(&message);
    CHECK(!strcmp(message.requestId, "marker"));
    LiteMsgClear(&message);

    publish(&mock, 1, "2026-09-08 10:01:00");
    MessageBusPopInbound(&message);
    CHECK(strstr(message.content, "告警时间 ：2026-09-08 10:01:00,"));
    complete_request(&message);

    CHECK(!AlarmServiceStop(service, 2000));
    AlarmServiceDestroy(service);
    MessageBusDestroy();
    pthread_mutex_destroy(&mock.mu);
}

static void fill_bus(void) {
    for (int i = 0; i < LITE_NORMAL_QUEUE_LEN; i++) {
        LiteMsg message = {0};
        message.content = strdup("filler");
        CHECK(!MessageBusTryPushInbound(&message));
    }
}

static void empty_bus(int count) {
    for (int i = 0; i < count; i++) {
        LiteMsg message = {0};
        MessageBusPopInbound(&message);
        LiteMsgClear(&message);
    }
}

static void retry_test(void) {
    AlarmConfig config;
    Mock mock = {.mu = PTHREAD_MUTEX_INITIALIZER};
    AspAlarmClient client;
    MessageBusInit();
    AlarmService* service = start_service(&config, &mock, &client, 50);
    fill_bus();
    publish(&mock, 9, "2026-09-08 10:09:00");
    usleep(100000);
    empty_bus(2);
    LiteMsg message = {0};
    MessageBusPopInbound(&message);
    CHECK(strstr(message.content, "告警序列号 ：9,"));
    complete_request(&message);
    empty_bus(LITE_NORMAL_QUEUE_LEN - 2);
    CHECK(!AlarmServiceStop(service, 2000));
    AlarmServiceDestroy(service);
    MessageBusDestroy();
    pthread_mutex_destroy(&mock.mu);
}

static void stop_test(void) {
    AlarmConfig config;
    Mock mock = {.mu = PTHREAD_MUTEX_INITIALIZER, .delay = 300};
    AspAlarmClient client;
    MessageBusInit();
    AlarmService* service = start_service(&config, &mock, &client, 50);
    usleep(50000);
    CHECK(AlarmServiceStop(service, 0) == ETIMEDOUT);
    AlarmServiceDestroy(service);
    CHECK(!AlarmServiceStop(service, 2000));
    AlarmServiceDestroy(service);
    MessageBusDestroy();
    pthread_mutex_destroy(&mock.mu);
}

static void durable_restart_test(void) {
    char directory[] = "/tmp/litecrab-alarm-spool-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    AlarmConfig config;
    Mock firstMock = {.mu = PTHREAD_MUTEX_INITIALIZER};
    AspAlarmClient firstClient;
    MessageBusInit();
    AlarmService* first =
        start_service_at(&config, &firstMock, &firstClient, 50, directory);
    publish(&firstMock, 77, "2026-09-08 10:17:00");
    LiteMsg interrupted = {0};
    MessageBusPopInbound(&interrupted);
    CHECK(strstr(interrupted.content, "告警序列号 ：77,"));
    LiteMsgClear(&interrupted);
    CHECK(!AlarmServiceStop(first, 2000));
    AlarmServiceDestroy(first);
    pthread_mutex_destroy(&firstMock.mu);

    Mock secondMock = {.mu = PTHREAD_MUTEX_INITIALIZER};
    AspAlarmClient secondClient;
    AlarmService* second =
        start_service_at(&config, &secondMock, &secondClient, 50, directory);
    LiteMsg recovered = {0};
    MessageBusPopInbound(&recovered);
    CHECK(strstr(recovered.content, "告警序列号 ：77,"));
    complete_request(&recovered);
    CHECK(!AlarmServiceStop(second, 2000));
    AlarmServiceDestroy(second);
    pthread_mutex_destroy(&secondMock.mu);

    Mock thirdMock = {.mu = PTHREAD_MUTEX_INITIALIZER};
    AspAlarmClient thirdClient;
    AlarmService* third =
        start_service_at(&config, &thirdMock, &thirdClient, 50, directory);
    publish(&thirdMock, 77, "2026-09-08 10:17:00");
    LiteMsg marker = {0};
    strcpy(marker.requestId, "durable-marker");
    marker.content = strdup("marker");
    CHECK(!MessageBusTryPushInbound(&marker));
    MessageBusPopInbound(&marker);
    CHECK(!strcmp(marker.requestId, "durable-marker"));
    LiteMsgClear(&marker);
    CHECK(!AlarmServiceStop(third, 2000));
    AlarmServiceDestroy(third);
    pthread_mutex_destroy(&thirdMock.mu);
    MessageBusDestroy();

    char spool[512];
    snprintf(spool, sizeof spool, "%s/alarm_spool.bin", directory);
    struct stat status;
    CHECK(!stat(spool, &status));
    CHECK(status.st_size > 0 && status.st_size <= 8 * 1024 * 1024);
    unlink(spool);
    rmdir(directory);
}

static void handled_barrier_restart_test(void) {
    char directory[] = "/tmp/litecrab-alarm-handled-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    AlarmConfig config;
    Mock firstMock = {.mu = PTHREAD_MUTEX_INITIALIZER};
    AspAlarmClient firstClient;
    MessageBusInit();
    AlarmService* service =
        start_service_at(&config, &firstMock, &firstClient, 20, directory);
    publish(&firstMock, 78, "2026-09-08 10:17:01");
    LiteMsg interrupted = {0};
    MessageBusPopInbound(&interrupted);
    LiteMsgClear(&interrupted);
    CHECK(!AlarmServiceStop(service, 2000));
    AlarmServiceDestroy(service);
    pthread_mutex_destroy(&firstMock.mu);

    /* Model a crash after the HANDLED replay barrier reached disk but before
     * ACK-history compaction.  The v3 header is 24 bytes and handled is the
     * third int32 field of record zero. */
    char spool[512];
    snprintf(spool, sizeof spool, "%s/alarm_spool.bin", directory);
    int fd = open(spool, O_RDWR);
    CHECK(fd >= 0);
    if (fd >= 0) {
        char magic[8];
        uint32_t version = 0;
        int32_t handled = 1;
        CHECK(pread(fd, magic, sizeof magic, 0) == (ssize_t) sizeof magic);
        CHECK(!memcmp(magic, "LCRALM3", 8));
        CHECK(pread(fd, &version, sizeof version, 8) == (ssize_t) sizeof version);
        CHECK(version == 3);
        CHECK(pwrite(fd, &handled, sizeof handled, 32) == (ssize_t) sizeof handled);
        CHECK(!fsync(fd));
        close(fd);
    }

    Mock restoredMock = {.mu = PTHREAD_MUTEX_INITIALIZER};
    AspAlarmClient restoredClient;
    service = start_service_at(&config, &restoredMock, &restoredClient, 20, directory);
    usleep(100000);
    publish(&restoredMock, 78, "2026-09-08 10:17:01");
    usleep(100000);
    LiteMsg marker = {0};
    strcpy(marker.requestId, "handled-barrier-marker");
    marker.content = strdup("marker");
    CHECK(!MessageBusTryPushInbound(&marker));
    MessageBusPopInbound(&marker);
    CHECK(!strcmp(marker.requestId, "handled-barrier-marker"));
    LiteMsgClear(&marker);
    CHECK(!AlarmServiceStop(service, 2000));
    AlarmServiceDestroy(service);
    pthread_mutex_destroy(&restoredMock.mu);
    MessageBusDestroy();
    unlink(spool);
    rmdir(directory);
}

static void retry_exhaustion_persists_dead_letter_test(void) {
    char directory[] = "/tmp/litecrab-alarm-dead-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    AlarmConfig config;
    Mock mock = {.mu = PTHREAD_MUTEX_INITIALIZER};
    AspAlarmClient client;
    MessageBusInit();
    AlarmService* service = start_service_at(&config, &mock, &client, 20, directory);
    publish(&mock, 88, "2026-09-08 10:18:00");
    for (int attempt = 0; attempt < 1 + ALARM_FORWARD_RETRIES; attempt++) {
        LiteMsg request = {0};
        MessageBusPopInbound(&request);
        CHECK(strstr(request.content, "告警序列号 ：88,"));
        complete_request_status(&request, LITE_COMPLETION_RETRYABLE);
    }
    usleep(80000);
    CHECK(!AlarmServiceStop(service, 2000));
    AlarmServiceDestroy(service);
    pthread_mutex_destroy(&mock.mu);

    Mock restoredMock = {.mu = PTHREAD_MUTEX_INITIALIZER};
    AspAlarmClient restoredClient;
    service = start_service_at(&config, &restoredMock, &restoredClient, 20, directory);
    LiteMsg marker = {0};
    strcpy(marker.requestId, "dead-letter-marker");
    marker.content = strdup("marker");
    CHECK(!MessageBusTryPushInbound(&marker));
    MessageBusPopInbound(&marker);
    CHECK(!strcmp(marker.requestId, "dead-letter-marker"));
    LiteMsgClear(&marker);
    CHECK(!AlarmServiceStop(service, 2000));
    AlarmServiceDestroy(service);
    pthread_mutex_destroy(&restoredMock.mu);
    MessageBusDestroy();

    char spool[512];
    snprintf(spool, sizeof spool, "%s/alarm_spool.bin", directory);
    unlink(spool);
    rmdir(directory);
}

static void spool_failure_never_dispatches_test(void) {
    AlarmConfig config;
    Mock mock = {.mu = PTHREAD_MUTEX_INITIALIZER};
    AspAlarmClient client;
    MessageBusInit();
    /* /proc is a directory but cannot host the atomic spool file. */
    AlarmService* service = start_service_at(&config, &mock, &client, 20, "/proc");
    publish(&mock, 99, "2026-09-08 10:19:00");
    LiteMsg marker = {0};
    strcpy(marker.requestId, "spool-failure-marker");
    marker.content = strdup("marker");
    CHECK(!MessageBusTryPushInbound(&marker));
    MessageBusPopInbound(&marker);
    CHECK(!strcmp(marker.requestId, "spool-failure-marker"));
    LiteMsgClear(&marker);
    CHECK(!AlarmServiceStop(service, 2000));
    AlarmServiceDestroy(service);
    MessageBusDestroy();
    pthread_mutex_destroy(&mock.mu);
}

int main(void) {
    parse_tests();
    delivery_and_dedup_test();
    retry_test();
    stop_test();
    durable_restart_test();
    handled_barrier_restart_test();
    retry_exhaustion_persists_dead_letter_test();
    spool_failure_never_dispatches_test();
    fprintf(stderr, "alarm checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
