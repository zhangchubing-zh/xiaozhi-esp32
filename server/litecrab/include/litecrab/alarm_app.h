#ifndef LITECRAB_ALARM_APP_H
#define LITECRAB_ALARM_APP_H
#include "litecrab/alarm.h"
typedef struct {
    int enabled;
    AlarmConfig config;
} AlarmAppOptions;
typedef struct {
    AlarmService* service;
    AspAlarmClient* client;
} AlarmApp;
void AlarmAppDefaults(AlarmAppOptions* options);
/* 1 consumed, 0 unrelated, -1 invalid; keeps alarm CLI/config out of main. */
int AlarmAppParseArg(AlarmAppOptions* options, int argc, char** argv, int* index);
int AlarmAppStart(AlarmApp* app, const AlarmAppOptions* options);
int AlarmAppQuiesce(AlarmApp* app);
int AlarmAppStop(AlarmApp* app);
#endif
