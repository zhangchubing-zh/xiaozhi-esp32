#ifndef PLC_ASP_H
#define PLC_ASP_H
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BASE_URL_DEFAULT       "https://192.168.8.10"
#define LOGIN_PATH             "/action/login"
#define CACHE_INFO_PATH        "/get_cache_info.asp"
#define MONITOR_INFO_PATH      "/get_monitor_info.asp"
#define HISTORY_INFO_PATH      "/get_history_info.asp"
#define SET_SIGNAL_PATH        "/set_signal_info.asp"
#define ENV_USER               "LITECRAB_ALARM_USER"
#define ENV_PASSWORD           "LITECRAB_ALARM_PASSWORD"
#define DEFAULT_LANGLIST       "zh-cn"
#define DEFAULT_EQUIP_ID       "4099"
#define DEFAULT_EQUIP_TYPE_ID  "33036"
#define DEFAULT_PARA3          "4"   /* 4=设定值组(含8201频段); 5=另一组 */
#define DEFAULT_PARA4          "2"
#define DEFAULT_HISTORY_EQUIP  "0"
#define DEFAULT_PAGE_INDEX     "1"
#define DEFAULT_PAGE_SIZE      "20"
#define DEFAULT_ALARM_LEVEL    "255"
#define DEFAULT_SORT_TYPE      "0"
#define DEFAULT_SORT_FIELD     "0"
#define FREQ_BAND_SIG_ID        8201
#define RESPONSE_INITIAL       4096U
#define RESPONSE_MAX           (1024U * 1024U)
#define TOKEN_MAX              256U

enum { OK = 0, ARG_ERROR = 1, LOCAL_ERROR = 2, NETWORK_ERROR = 3,
       HTTP_ERROR = 4, AUTH_ERROR = 5, DIAGNOSIS_ERROR = 6 };

typedef struct {
    const char *operation;
    const char *user, *password, *langlist, *base_url;
    const char *equip_id, *equip_type_id, *para3, *para4;
    const char *history_equip_id;
    const char *start_time, *end_time, *page_index, *page_size;
    const char *alarm_level, *sort_type, *sort_field, *output;
    const char *value;   /* set_freq_band 用 */
} Options;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
    long http_status;
} Buffer;

typedef struct {
    int before_band;
    int target_band;
    int after_band;
    const char *target_value;
    long before_http_status;
    long write_http_status;
    long after_http_status;
    int write_business_code;
    int device_result;
    int signal_result;
    int write_attempted;
    int write_result_known;
    int readback_matches;
    const char *failed_stage;
} FrequencyBandResult;

size_t receive(void *, size_t, size_t, void *);
void json_string(const char *);
void apply_test_tls(CURL *);
char *do_login(CURL *, const Options *, Buffer *);
int do_get(CURL *, const char *, const char *, Buffer *);
int do_post(CURL *, const char *, const char *, const char *, const char *, Buffer *);
int save_response(const char *, const Buffer *);
int asp_get_cache_info(CURL *, const char *, const Options *, Buffer *);
int asp_get_monitor_info(CURL *, const char *, const Options *, Buffer *);
int asp_get_history_alarm(CURL *, const char *, const Options *, Buffer *);
int asp_get_active_alarm(CURL *, const char *, const Options *, Buffer *);
int asp_set_freq_band(CURL *, const char *, const Options *, Buffer *);
void normalized_print_error(const char *, int, long, const char *, const char *);
void normalized_print_login(int, long);
void normalized_print_cache(const Options *, const Buffer *, int);
void normalized_print_active_alarm(const Options *, const Buffer *, int);
void normalized_print_monitor_info(const Options *, const Buffer *, int);
void normalized_print_history_alarm(const Options *, const Buffer *, int);
int normalized_parse_set_result(const Buffer *, const Options *, FrequencyBandResult *);
void normalized_print_frequency_band(const Options *, const FrequencyBandResult *, int);
void normalized_print_history_alarm(const Options *, const Buffer *, int);
void normalized_print_frequency_band(const Options *, const FrequencyBandResult *, int);
int normalized_parse_set_result(const Buffer *, const Options *, FrequencyBandResult *);
#endif
