#include "asp.h"
/* 构造 set_freq_band 的 POST body: para=<urlencode({"type":1,"equipList":[{...}]})> */
static char *build_set_freq_body(CURL *curl, const Options *o)
{
    char json[512];
    int n = snprintf(json, sizeof json,
        "{\"type\":1,\"equipList\":[{\"equipTypeId\":%s,\"equipId\":%s,"
        "\"sigList\":[{\"sigId\":%d,\"sigValue\":\"%s\"}]}]}",
        o->equip_type_id, o->equip_id, FREQ_BAND_SIG_ID, o->value);
    if (n <= 0 || (size_t)n >= sizeof json) return NULL;
    char *enc = curl_easy_escape(curl, json, 0);
    if (!enc) return NULL;
    char *body = malloc(strlen(enc) + 8);
    if (!body) { curl_free(enc); return NULL; }
    strcpy(body, "para=");
    strcat(body, enc);
    curl_free(enc);
    return body;
}

int asp_set_freq_band(CURL *c, const char *t, const Options *o, Buffer *r) {
 char url[2048];
 int n=snprintf(url,sizeof url,"%s%s",o->base_url,SET_SIGNAL_PATH);
 if(n<0 || (size_t)n>=sizeof url) return LOCAL_ERROR;
 char *body=build_set_freq_body(c,o);
 if(!body) return LOCAL_ERROR;
 int rc=do_post(c,t,url,body,"set_freq_band",r);
 free(body); return rc;
}
