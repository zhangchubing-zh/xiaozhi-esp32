#include "asp.h"
#include "litecrab/json.h"

static int active_alarm_response_ok(const Buffer *r)
{
    if (LjValidate(r->data, LJ_OBJECT)) return 0;
    LjToken *t = calloc(r->size + 1, sizeof *t);
    if (!t) return 0;
    LjParser parser;
    LjInit(&parser);
    int n = LjParse(&parser, r->data, r->size, t, (unsigned)(r->size + 1));
    int code = LjObjectGet(r->data, t, n, 0, "errcode");
    int list = LjObjectGet(r->data, t, n, 0, "almlist");
    int64_t number;
    int ok = code >= 0 && list >= 0 && t[list].type == LJ_ARRAY &&
        (LjTokenEq(r->data, &t[code], "OK") || (LjInt64(r->data, &t[code], &number) == 0 && number == 0));
    free(t);
    return ok;
}

/* docs/ASP/10: Type=21, para1 is URL-encoded JSON; errcode is "OK". */
int asp_get_active_alarm(CURL *c, const char *t, const Options *o, Buffer *r)
{
    char json[256], url[2048];
    int n = snprintf(json, sizeof json, "{\"equipid\":%s,\"equipidlist\":[]}", o->history_equip_id);
    if (n < 0 || (size_t)n >= sizeof json) return LOCAL_ERROR;
    char *encoded = curl_easy_escape(c, json, 0);
    if (!encoded) return LOCAL_ERROR;
    n = snprintf(url, sizeof url, "%s%s?type=21&para1=%s&para2=&para3=&para4=&para5=&para6=",
                 o->base_url, MONITOR_INFO_PATH, encoded);
    curl_free(encoded);
    if (n < 0 || (size_t)n >= sizeof url) return LOCAL_ERROR;
    int rc = do_get(c, t, url, r);
    if (rc == OK && !active_alarm_response_ok(r)) return HTTP_ERROR;
    return rc;
}
