#include "monitor.h"
#include "litecrab/json.h"
#include "litecrab/observability.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    AlarmConfig cfg;
    CURLM *multi;
    CURL *easy;
    curl_mime *form;
    struct curl_slist *headers;
    atomic_int cancelled;
    int loggedIn, attached, phase; /* 1 login, 2 subscription, 3 active query */
    char body[ALARM_MAX_TEXT_LEN], token[256], url[1024];
    size_t size;
} CurlClient;
static pthread_once_t curlOnce=PTHREAD_ONCE_INIT;
static int curlInitResult;
static void init_curl(void){curlInitResult=(int)curl_global_init(CURL_GLOBAL_DEFAULT);}
static size_t receive(void *data,size_t size,size_t count,void *arg) {
    CurlClient *c=arg;
    if(count&&size>SIZE_MAX/count)return 0;
    size_t n=size*count;
    if(n>=sizeof c->body||c->size>sizeof c->body-1-n)return 0;
    memcpy(c->body+c->size,data,n);c->size+=n;c->body[c->size]=0;
    return n;
}
static void detach(CurlClient *c) {
    if(c->attached){curl_multi_remove_handle(c->multi,c->easy);c->attached=0;}
    curl_easy_setopt(c->easy,CURLOPT_HTTPHEADER,NULL);
    curl_easy_setopt(c->easy,CURLOPT_MIMEPOST,NULL);
    curl_slist_free_all(c->headers);c->headers=NULL;
    if(c->form){curl_mime_free(c->form);c->form=NULL;}
}
static int launch(CurlClient *c) {
    detach(c);c->size=0;c->body[0]=0;
    c->phase=c->loggedIn?(c->cfg.mode==ALARM_MODE_SUBSCRIPTION?2:3):1;
    const char *path=c->phase==1?"/action/login":
                     (c->phase==2?AlarmSubscriptionPath():AlarmPollingPath());
    snprintf(c->url,sizeof c->url,"%s%s",c->cfg.baseUrl,path);
    curl_easy_setopt(c->easy,CURLOPT_URL,c->url);
    curl_easy_setopt(c->easy,CURLOPT_TIMEOUT_MS,
                     c->phase==2?(long)c->cfg.longPollDeadlineMs:15000L);
    if(c->loggedIn) {
        char auth[300];snprintf(auth,sizeof auth,"x-csrf-token: %s",c->token);
        c->headers=curl_slist_append(NULL,auth);
        if(!c->headers)return -1;
        curl_easy_setopt(c->easy,CURLOPT_HTTPGET,1L);
        curl_easy_setopt(c->easy,CURLOPT_HTTPHEADER,c->headers);
    } else {
        c->form=curl_mime_init(c->easy);if(!c->form)return -1;
        const char *names[]={"usrname","string","langlist"};
        const char *values[]={c->cfg.user,c->cfg.password,c->cfg.langlist};
        for(int i=0;i<3;i++) {
            curl_mimepart *p=curl_mime_addpart(c->form);
            if(!p||curl_mime_name(p,names[i])||curl_mime_data(p,values[i],CURL_ZERO_TERMINATED))return -1;
        }
        curl_easy_setopt(c->easy,CURLOPT_MIMEPOST,c->form);
    }
    if(curl_multi_add_handle(c->multi,c->easy)!=CURLM_OK)return -1;
    c->attached=1;return 0;
}
static int begin(AspAlarmClient *self,uint64_t generation) {
    (void)generation;CurlClient *c=self->impl;
    atomic_store(&c->cancelled,0);
    return launch(c); /* no blocking login or discarded registration response */
}
static int poll_client(AspAlarmClient *self,uint32_t maxWaitMs,AlarmNotify *out) {
    CurlClient *c=self->impl;
    if(atomic_load(&c->cancelled))return ALARM_POLL_IDLE;
    int running=0;
    if(curl_multi_perform(c->multi,&running)!=CURLM_OK){LogPrint("[alarm] curl perform failed");return ALARM_POLL_NETWORK_ERROR;}
    if(running) {
        int ready=0;int wait=maxWaitMs>100?100:(int)maxWaitMs;
        if(curl_multi_poll(c->multi,NULL,0,wait,&ready)!=CURLM_OK){LogPrint("[alarm] curl poll failed");return ALARM_POLL_NETWORK_ERROR;}
        if(atomic_load(&c->cancelled))return ALARM_POLL_IDLE;
        if(curl_multi_perform(c->multi,&running)!=CURLM_OK){LogPrint("[alarm] curl perform failed");return ALARM_POLL_NETWORK_ERROR;}
    }
    int left;CURLMsg *msg;
    while((msg=curl_multi_info_read(c->multi,&left))) {
        if(msg->msg!=CURLMSG_DONE)continue;
        CURLcode result=msg->data.result;
        long status=0;curl_easy_getinfo(c->easy,CURLINFO_RESPONSE_CODE,&status);
        int phase=c->phase;detach(c);
        if(result==CURLE_OPERATION_TIMEDOUT&&phase==2)return ALARM_POLL_RESPONSE_END;
        if(result!=CURLE_OK){LogPrint("[alarm] HTTP request failed: %s",curl_easy_strerror(result));return ALARM_POLL_NETWORK_ERROR;}
        if(status==401||status==403){c->loggedIn=0;return ALARM_POLL_AUTH_EXPIRED;}
        if(status<200||status>=300){
            LogPrint("[alarm] HTTP request returned status=%ld; re-authenticating",status);
            c->loggedIn=0;
            return ALARM_POLL_AUTH_EXPIRED;
        }
        if(phase==1) {
            LjToken t[128];LjParser p;LjInit(&p);
            int n=LjParse(&p,c->body,c->size,t,128);
            int k=n>0?LjObjectGet(c->body,t,n,0,"token"):-1;
            if(k<0||LjString(c->body,&t[k],c->token,sizeof c->token)||!c->token[0]){LogPrint("[alarm] login response has no valid token");return ALARM_POLL_AUTH_EXPIRED;}
            if(strpbrk(c->token,"\r\n"))return ALARM_POLL_AUTH_EXPIRED;
            c->loggedIn=1;
            return launch(c)?ALARM_POLL_NETWORK_ERROR:ALARM_POLL_IDLE;
        }
        if(phase==2) {
            /* A subscription notification is only a trigger. Always fetch the
             * canonical active-alarm list before forwarding anything. */
            AlarmMode configured=c->cfg.mode;
            c->cfg.mode=ALARM_MODE_POLLING;
            int launched=launch(c);
            c->cfg.mode=configured;
            return launched?ALARM_POLL_NETWORK_ERROR:ALARM_POLL_IDLE;
        }
        if(!c->size)return ALARM_POLL_NETWORK_ERROR;
        if(!strcmp(c->body,"ERR")||strstr(c->body,"<html")){c->loggedIn=0;return ALARM_POLL_AUTH_EXPIRED;}
        LjToken *tokens=calloc(c->size+1,sizeof *tokens);
        if(!tokens)return ALARM_POLL_NETWORK_ERROR;
        LjParser parser;LjInit(&parser);
        int count=LjParse(&parser,c->body,c->size,tokens,(unsigned)c->size+1);
        int list=count>0?LjObjectGet(c->body,tokens,count,0,"almlist"):-1;
        int code=count>0?LjObjectGet(c->body,tokens,count,0,"errcode"):-1;
        int64_t codeNumber=0;
        int valid=list>=0&&tokens[list].type==LJ_ARRAY&&code>=0&&
                  (LjTokenEq(c->body,&tokens[code],"OK")||
                   (!LjInt64(c->body,&tokens[code],&codeNumber)&&codeNumber==0));
        int alarms=valid?tokens[list].size:0;
        free(tokens);
        if(!valid||AlarmNotifyParse(c->body,out)){LogPrint("[alarm] invalid active alarm response bytes=%zu",c->size);return ALARM_POLL_NETWORK_ERROR;}
        LogPrint("[alarm] active query ok mode=%s alarms=%d bytes=%zu",
                 c->cfg.mode==ALARM_MODE_SUBSCRIPTION?"subscription":"polling",alarms,c->size);
        return ALARM_POLL_NOTIFY;
    }
    return ALARM_POLL_IDLE;
}
static int cancel(AspAlarmClient *self) {
    atomic_store(&((CurlClient*)self->impl)->cancelled,1);return 0;
}
static void destroy(AspAlarmClient *self) {
    if(!self)return;
    CurlClient *c=self->impl;
    if(c){if(c->easy)detach(c);if(c->easy)curl_easy_cleanup(c->easy);
        if(c->multi)curl_multi_cleanup(c->multi);
        memset(c->cfg.password,0,sizeof c->cfg.password);free(c);}
    free(self);
}
static const AspAlarmClientVTable vtable={begin,poll_client,cancel,destroy};
AspAlarmClient *AspAlarmClientCurlCreate(const AlarmConfig *cfg) {
    if(!cfg||!cfg->baseUrl[0]||!cfg->user[0]||!cfg->password[0])return NULL;
    if(strncmp(cfg->baseUrl,"https://",8)&&strncmp(cfg->baseUrl,"http://",7))return NULL;
    pthread_once(&curlOnce,init_curl);if(curlInitResult)return NULL;
    AspAlarmClient *self=calloc(1,sizeof *self);CurlClient *c=calloc(1,sizeof *c);
    if(!self||!c){free(self);free(c);return NULL;}
    self->vtable=&vtable;self->impl=c;c->cfg=*cfg;
    if(c->cfg.longPollDeadlineMs<=0)c->cfg.longPollDeadlineMs=120000;
    c->multi=curl_multi_init();c->easy=curl_easy_init();
    if(!c->multi||!c->easy){destroy(self);return NULL;}
    curl_easy_setopt(c->easy,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c->easy,CURLOPT_COOKIEFILE,"");
    curl_easy_setopt(c->easy,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(c->easy,CURLOPT_WRITEDATA,c);
    curl_easy_setopt(c->easy,CURLOPT_CONNECTTIMEOUT_MS,5000L);
    curl_easy_setopt(c->easy,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(c->easy,CURLOPT_SSL_VERIFYPEER,cfg->insecureTls?0L:1L);
    curl_easy_setopt(c->easy,CURLOPT_SSL_VERIFYHOST,cfg->insecureTls?0L:2L);
    if(cfg->caFile[0])curl_easy_setopt(c->easy,CURLOPT_CAINFO,cfg->caFile);
    return self;
}
