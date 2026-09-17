#include "litecrab/alarm_app.h"
#include "litecrab/config.h"
#include "litecrab/gateway.h"
#include "litecrab/kernel.h"
#include "litecrab/observability.h"
#include "litecrab/xiaozhi_ws.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LITECRAB_BUILD_REVISION
#define LITECRAB_BUILD_REVISION "unknown"
#endif
#ifndef LITECRAB_BUILD_TIME_UTC
#define LITECRAB_BUILD_TIME_UTC "unknown"
#endif

static void stop_server(int sig) {
    (void) sig;
    RequestServerRequestStop();
    XiaozhiWsRequestStop();
}

static void usage(const char* program) {
    fprintf(stderr,
            "Usage: %s [--config FILE] [--llm-config FILE] [--llm-provider NAME] "
            "[--workspace DIR] [--port PORT] [--alarm-mode polling|subscription] "
            "[--alarm-base-url URL] [--alarm-user USER] [--alarm-password PWD] "
            "[--alarm-ca-file FILE] [--alarm-insecure-tls] [--alarm|--no-alarm]\n",
            program);
}

static int parse_port(const char* text, int* out) {
    char* end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || !end || *end || value < 1 || value > 65535)
        return -1;
    *out = (int) value;
    return 0;
}

int main(int argc, char** argv) {
    const char *baseConfig = NULL, *llmConfig = NULL, *llmProvider = NULL, *workspace = NULL,
               *portText = NULL;
    AlarmAppOptions alarmOptions;
    AlarmAppDefaults(&alarmOptions);
    for (int i = 1; i < argc; i++) {
        int alarmArg = AlarmAppParseArg(&alarmOptions, argc, argv, &i);
        if (alarmArg < 0) {
            usage(argv[0]);
            return 2;
        }
        if (alarmArg > 0)
            continue;
        if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if ((!strcmp(argv[i], "--config") || !strcmp(argv[i], "--llm-config") ||
             !strcmp(argv[i], "--llm-provider") || !strcmp(argv[i], "--workspace") ||
             !strcmp(argv[i], "--port")) &&
            i + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        if (!strcmp(argv[i], "--config"))
            baseConfig = argv[++i];
        else if (!strcmp(argv[i], "--llm-config"))
            llmConfig = argv[++i];
        else if (!strcmp(argv[i], "--llm-provider"))
            llmProvider = argv[++i];
        else if (!strcmp(argv[i], "--workspace"))
            workspace = argv[++i];
        else if (!strcmp(argv[i], "--port"))
            portText = argv[++i];
        else if (argv[i][0] == '-') {
            usage(argv[0]);
            return 2;
        } else if (!workspace)
            workspace = argv[i];
        else if (!portText)
            portText = argv[i];
        else {
            usage(argv[0]);
            return 2;
        }
    }
    LiteCrabAppConfig app;
    char error[512];
    if (LiteCrabConfigLoadProvider(&app, baseConfig, llmConfig, llmProvider, error, sizeof error)) {
        fprintf(stderr, "configuration error: %s\n", error);
        return 2;
    }
    if (workspace)
        snprintf(app.workspaceRoot, sizeof app.workspaceRoot, "%s", workspace);
    if (!alarmOptions.config.spoolDirectory[0])
        if (snprintf(alarmOptions.config.spoolDirectory,
                     sizeof alarmOptions.config.spoolDirectory,
                     "%s/.crab/alarm",
                     app.workspaceRoot) >= (int) sizeof alarmOptions.config.spoolDirectory) {
            fprintf(stderr, "Alarm spool path is too long\n");
            return 2;
        }
    if (portText && parse_port(portText, &app.server.listenPort)) {
        fprintf(stderr, "invalid port: %s\n", portText);
        return 2;
    }
    CrabRuntimeConfig runtime = {.workspaceRoot = app.workspaceRoot, .rawResultDir = ".crab/raw"};
    if (InitLogWithDir(app.logDir)) {
        fprintf(stderr, "failed to initialize logs\n");
        return 1;
    }
    LogPrint("[startup] revision=%s build_time_utc=%s workspace=%s limits={connections:%d,workers:%d,request_bytes:%d}",
             LITECRAB_BUILD_REVISION,
             LITECRAB_BUILD_TIME_UTC,
             app.workspaceRoot,
             app.server.maxConnections,
             app.server.workerThreads,
             app.server.maxReqBytes);
    if (AgentLoopInit(&runtime, &app.llm) || AgentLoopStart()) {
        fprintf(stderr, "failed to initialize LiteCrab\n");
        CloseLog();
        return 1;
    }
    AlarmApp alarmApp;
    if (AlarmAppStart(&alarmApp, &alarmOptions)) {
        fprintf(stderr, "alarm startup failed; check configuration and credentials\n");
        AgentLoopStop();
        CloseLog();
        return 1;
    }
    struct sigaction stopAction = {0};
    stopAction.sa_handler = stop_server;
    sigemptyset(&stopAction.sa_mask);
    sigaction(SIGINT, &stopAction, NULL);
    sigaction(SIGTERM, &stopAction, NULL);
    if (app.xiaozhiWs.enabled) {
        if (XiaozhiWsStart(&app.xiaozhiWs)) {
            fprintf(stderr, "xiaozhi websocket gateway startup failed\n");
            AgentLoopStop();
            AlarmAppStop(&alarmApp);
            CloseLog();
            return 1;
        }
    } else {
        LogPrint("[gateway] xiaozhi-ws disabled (base_config.json xiaozhi_ws.enabled=false)");
    }
    if (app.server.allowUnauthenticatedRemote)
        LogPrint("[security] WARNING unauthenticated remote TCP is explicitly enabled; "
                 "request userId is untrusted; bind=%s:%d",
                 app.server.listenIp,
                 app.server.listenPort);
    else
        LogPrint("[security] TCP gateway has no client authentication; request userId is "
                 "untrusted; exposure=loopback bind=%s:%d",
                 app.server.listenIp,
                 app.server.listenPort);
    LogPrint("[gateway] LiteCrab starting on %s:%d", app.server.listenIp, app.server.listenPort);
    int rc = StartRequestServer(&app.server, LiteCrabHandleNetworkRequest, NULL);
    XiaozhiWsStop();
    int alarmStop = AlarmAppQuiesce(&alarmApp);
    AgentLoopStop();
    if (AlarmAppStop(&alarmApp) || alarmStop)
        rc = -1;
    CloseLog();
    return rc ? 1 : 0;
}
