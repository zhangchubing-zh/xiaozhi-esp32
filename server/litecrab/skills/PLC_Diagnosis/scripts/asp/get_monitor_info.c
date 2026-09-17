#include "asp.h"
#include "litecrab/json.h"

static int monitor_response_ok(const Buffer *response)
{
    if (!response || !response->data || LjValidate(response->data, LJ_OBJECT)) return 0;
    LjToken *tokens = calloc(response->size + 1, sizeof *tokens);
    if (!tokens) return 0;
    LjParser parser;
    LjInit(&parser);
    int count = LjParse(&parser, response->data, response->size, tokens,
                        (unsigned)(response->size + 1));
    int code = count > 0 ? LjObjectGet(response->data, tokens, count, 0, "errCode") : -1;
    int list = count > 0 ? LjObjectGet(response->data, tokens, count, 0, "sigList") : -1;
    int64_t number = -1;
    int ok = code >= 0 && list >= 0 && tokens[list].type == LJ_ARRAY &&
             LjInt64(response->data, &tokens[code], &number) == 0 && number == 0;
    free(tokens);
    return ok;
}

/* 构造 get_monitor_info URL: type=13&para1=equipId&para2=equipTypeId&para3&para4 */
static int build_monitor_url(CURL *curl, const Options *o, char *out, size_t outsz)
{
    char *e = curl_easy_escape(curl, o->equip_id, 0);
    char *t = curl_easy_escape(curl, o->equip_type_id, 0);
    char *p3 = curl_easy_escape(curl, o->para3, 0);
    char *p4 = curl_easy_escape(curl, o->para4, 0);
    int n = snprintf(out, outsz,
        "%s%s?type=13&para1=%s&para2=%s&para3=%s&para4=%s&para5=&para6=",
        o->base_url, MONITOR_INFO_PATH, e, t, p3, p4);
    curl_free(e); curl_free(t); curl_free(p3); curl_free(p4);
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}

int asp_get_monitor_info(CURL *c, const char *t, const Options *o, Buffer *r) {
 char url[2048];
 if (build_monitor_url(c,o,url,sizeof url)) return LOCAL_ERROR;
    int rc = do_get(c,t,url,r);
    if (rc == OK && !monitor_response_ok(r)) return HTTP_ERROR;
 return rc;
}
