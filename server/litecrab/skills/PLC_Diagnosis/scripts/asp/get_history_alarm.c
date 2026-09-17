#include "asp.h"
static int format_local_time(time_t value, char out[20])
{
    struct tm result;
    if (!localtime_r(&value, &result)) return -1;
    return strftime(out, 20U, "%Y-%m-%d-%H-%M-%S", &result) == 19U ? 0 : -1;
}

/* Verified against the current historical-alarm frontend bundle:
 * GET /get_history_info.asp?type=7&para1=<URL-encoded JSON>.
 * The current UI uses YYYY-MM-DD-HH-mm-ss, not epoch seconds. */
static int build_history_url(CURL *curl, const Options *o, char *out, size_t outsz)
{
    char start_default[20], end_default[20], json[768];
    const char *start = o->start_time;
    const char *end = o->end_time;
    time_t now = time(NULL);
    if ((!start || !end) && now == (time_t)-1) return -1;
    if (!start) {
        time_t year_ago = now - (time_t)(365L * 24L * 60L * 60L);
        if (format_local_time(year_ago, start_default) != 0) return -1;
        start = start_default;
    }
    if (!end) {
        if (format_local_time(now, end_default) != 0) return -1;
        end = end_default;
    }
    int n = snprintf(json, sizeof json,
        "{\"equipid\":%s,\"startime\":\"%s\",\"endtime\":\"%s\","
        "\"sortype\":%s,\"alarmlevel\":%s,\"sortfield\":%s,"
        "\"pageindex\":%s,\"querynum\":%s,\"numperpage\":%s}",
        o->history_equip_id, start, end, o->sort_type, o->alarm_level, o->sort_field,
        o->page_index, o->page_size, o->page_size);
    if (n <= 0 || (size_t)n >= sizeof json) return -1;
    char *encoded = curl_easy_escape(curl, json, 0);
    if (!encoded) return -1;
    n = snprintf(out, outsz, "%s%s?type=7&para1=%s",
                 o->base_url, HISTORY_INFO_PATH, encoded);
    curl_free(encoded);
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}

static bool history_response_ok(const Buffer *resp)
{
    const char *key = "\"errcode\"";
    const char *p = resp && resp->data ? strstr(resp->data, key) : NULL;
    if (!p) return false;
    p += strlen(key);
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p++ != ':') return false;
    while (*p && isspace((unsigned char)*p)) ++p;
    return *p == '0';
}

int asp_get_history_alarm(CURL *c, const char *t, const Options *o, Buffer *r) {
 char url[4096];
 if (build_history_url(c,o,url,sizeof url)) return LOCAL_ERROR;
 int rc=do_get(c,t,url,r);
 if (rc==OK && !history_response_ok(r)) return HTTP_ERROR;
 return rc;
}
