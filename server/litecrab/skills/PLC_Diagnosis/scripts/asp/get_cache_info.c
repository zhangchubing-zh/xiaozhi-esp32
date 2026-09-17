#include "asp.h"
int asp_get_cache_info(CURL *c, const char *t, const Options *o, Buffer *r) {
 char url[2048];
 int n=snprintf(url,sizeof url,"%s%s",o->base_url,CACHE_INFO_PATH);
 if(n<0 || (size_t)n>=sizeof url) return LOCAL_ERROR;
 return do_get(c,t,url,r);
}
