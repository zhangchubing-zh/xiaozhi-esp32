#ifndef LITECRAB_ALARM_MONITOR_H
#define LITECRAB_ALARM_MONITOR_H

#include "litecrab/alarm.h"

const char* AlarmPollingPath(void);
const char* AlarmSubscriptionPath(void);
int AlarmOccurrenceParse(const char* row, AlarmNotify* out);

#endif
