#include "litecrab/alarm_app.h"

#include "litecrab/observability.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int copy(char* out, size_t z, const char* s) {
    if (strlen(s) >= z)
        return -1;
    strcpy(out, s);
    return 0;
}
void AlarmAppDefaults(AlarmAppOptions* o) {
    memset(o, 0, sizeof *o);
    const char *url = getenv("LITECRAB_ALARM_URL"), *user = getenv("LITECRAB_ALARM_USER"),
               *pw = getenv("LITECRAB_ALARM_PASSWORD");
    if (url && *url) {
        copy(o->config.baseUrl, sizeof o->config.baseUrl, url);
    } else
        copy(o->config.baseUrl, sizeof o->config.baseUrl, "https://192.168.8.10");
    copy(o->config.user, sizeof o->config.user, user && *user ? user : "");
    if (pw && *pw)
        copy(o->config.password, sizeof o->config.password, pw);
    strcpy(o->config.langlist, "zh-cn");
    o->config.mode = ALARM_MODE_POLLING;
    o->config.pollIntervalMs = 5000;
    o->config.longPollDeadlineMs = 120000;
    o->config.ioTickMs = 100;
    o->config.retryDelayMs = 1000;
}
int AlarmAppParseArg(AlarmAppOptions* o, int argc, char** argv, int* index) {
    const char* arg = argv[*index];
    if (!strcmp(arg, "--alarm-mode")) {
        if (*index + 1 >= argc)
            return -1;
        const char* mode = argv[++*index];
        if (!strcmp(mode, "polling"))
            o->config.mode = ALARM_MODE_POLLING;
        else if (!strcmp(mode, "subscription"))
            o->config.mode = ALARM_MODE_SUBSCRIPTION;
        else
            return -1;
        o->enabled = 1;
        return 1;
    }
    if (!strcmp(arg, "--no-alarm")) {
        o->enabled = 0;
        return 1;
    }
    if (!strcmp(arg, "--alarm")) {
        o->enabled = 1;
        return 1;
    }
    if (!strcmp(arg, "--alarm-insecure-tls")) {
        o->config.insecureTls = 1;
        return 1;
    }
    char* out = NULL;
    size_t size = 0;
    if (!strcmp(arg, "--alarm-base-url")) {
        out = o->config.baseUrl;
        size = sizeof o->config.baseUrl;
    } else if (!strcmp(arg, "--alarm-user")) {
        out = o->config.user;
        size = sizeof o->config.user;
    } else if (!strcmp(arg, "--alarm-password")) {
        out = o->config.password;
        size = sizeof o->config.password;
    } else if (!strcmp(arg, "--alarm-ca-file")) {
        out = o->config.caFile;
        size = sizeof o->config.caFile;
    } else
        return 0;
    if (*index + 1 >= argc)
        return -1;
    return copy(out, size, argv[++*index]) ? -1 : 1;
}
int AlarmAppStart(AlarmApp* app, const AlarmAppOptions* o) {
    memset(app, 0, sizeof *app);
    if (!o->enabled)
        return 0;
    AlarmConfig cfg = o->config;
    if (!cfg.user[0] || !cfg.password[0])
        return -1;
    app->client = AspAlarmClientCurlCreate(&cfg);
    if (!app->client)
        return -1;
    cfg.client = app->client;
    if (AlarmServiceInit(&app->service, &cfg) || AlarmServiceStart(app->service)) {
        AlarmAppStop(app);
        return -1;
    }
    LogPrint("[alarm] started; mode=%s interval_ms=%d retries=3",
             cfg.mode == ALARM_MODE_SUBSCRIPTION ? "subscription" : "polling",
             cfg.pollIntervalMs);
    return 0;
}
int AlarmAppQuiesce(AlarmApp* app) {
    return AlarmServiceQuiesce(app->service, 5000);
}
int AlarmAppStop(AlarmApp* app) {
    int rc = AlarmServiceStop(app->service, 5000);
    if (rc)
        return rc;
    AlarmServiceDestroy(app->service);
    app->service = NULL;
    if (app->client)
        app->client->vtable->destroy(app->client);
    app->client = NULL;
    return 0;
}
