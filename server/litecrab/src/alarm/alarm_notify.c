#include "litecrab/alarm.h"
#include "litecrab/json.h"
#include <openssl/sha.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Canonical JSON: sorted object members, preserved array order, decoded strings.
 * This compares complete consecutive notifications, not a permanent alarm-ID set. */
static int canonical(const char *j, LjToken *t, int n, int k, LjBuf *b, int depth) {
    if (depth > 24) return -1;
    if (t[k].type == LJ_OBJECT) {
        int members[256], count=0;
        for (int i=k+1;i<n && t[i].start<t[k].end;) {
            if (count==256 || t[i].type!=LJ_STRING) return -1;
            members[count++]=i;
            i=LjSkip(t,n,i+1);
        }
        for(int a=1;a<count;a++) for(int x=a;x>0;x--) {
            char left[256],right[256];
            if(LjString(j,&t[members[x-1]],left,sizeof left)||LjString(j,&t[members[x]],right,sizeof right))return -1;
            if(strcmp(left,right)==0)return -1; /* ambiguous duplicate keys */
            if(strcmp(left,right)<0)break;
            int swap=members[x];members[x]=members[x-1];members[x-1]=swap;
        }
        LjAppend(b,"{");
        for(int i=0;i<count;i++) {
            if(i)LjAppend(b,",");
            if(canonical(j,t,n,members[i],b,depth+1))return -1;
            LjAppend(b,":");
            if(canonical(j,t,n,members[i]+1,b,depth+1))return -1;
        }
        LjAppend(b,"}");
    } else if(t[k].type==LJ_ARRAY) {
        LjAppend(b,"[");
        for(int i=0;i<t[k].size;i++) {
            if(i)LjAppend(b,",");
            int v=LjArrayGet(t,n,k,i);
            if(v<0||canonical(j,t,n,v,b,depth+1))return -1;
        }
        LjAppend(b,"]");
    } else if(t[k].type==LJ_STRING) {
        char text[ALARM_MAX_TEXT_LEN];
        if(LjString(j,&t[k],text,sizeof text))return -1;
        LjAppendJsonString(b,text);
    } else LjAppend(b,"%.*s",t[k].end-t[k].start,j+t[k].start);
    return b->failed?-1:0;
}
int AlarmNotifyParse(const char *raw, AlarmNotify *out) {
    if(!raw||!out||strlen(raw)>=ALARM_MAX_TEXT_LEN)return -1;
    memset(out,0,sizeof *out);
    while(isspace((unsigned char)*raw))raw++;
    size_t len=strlen(raw);
    while(len&&isspace((unsigned char)raw[len-1]))len--;
    if(!len)return -1;
    char clean[ALARM_MAX_TEXT_LEN];memcpy(clean,raw,len);clean[len]=0;
    char normalized[ALARM_MAX_TEXT_LEN*2];LjBuf b;LjBufInit(&b,normalized,sizeof normalized);
    int countFields=0,positive=0;
    const char *names[]={"CriticalNum","MajorNum","MinorNum","WarningNum","AllAlmNum"};
    if(clean[0]=='{') {
        if(LjValidate(clean,LJ_OBJECT))return -1;
        LjToken *t=calloc(len+1,sizeof *t);if(!t)return -1;
        LjParser p;LjInit(&p);int n=LjParse(&p,clean,len,t,(unsigned)len+1),bad=0;
        int error=LjObjectGet(clean,t,n,0,"errcode");int64_t errorNumber=0;
        if(error>=0&&!LjTokenEq(clean,&t[error],"OK")&&
           (LjInt64(clean,&t[error],&errorNumber)||errorNumber!=0))bad=1;
        for(int i=0;i<5;i++) {
            int k=LjObjectGet(clean,t,n,0,names[i]);int64_t value;
            if(k>=0) {
                if(LjInt64(clean,&t[k],&value)||value<0||value>1000000){bad=1;break;}
                countFields++;positive|=value>0;
            }
        }
        int list=LjObjectGet(clean,t,n,0,"almlist");
        if(list>=0){if(t[list].type!=LJ_ARRAY)bad=1;else{countFields++;positive|=t[list].size>0;}}
        if(!bad)bad=canonical(clean,t,n,0,&b,0);
        free(t);if(bad)return -1;
    } else {
        /* Explicit key=value~... text profile. Never interpret arbitrary HTML/ERR as an alarm. */
        for(int i=0;i<5;i++) {
            const char *v=strstr(clean,names[i]);
            if(!v)continue;
            if(v!=clean&&v[-1]!='~')continue;
            v+=strlen(names[i]);if(*v!='=')continue;
            char *end;long value=strtol(v+1,&end,10);
            if(end==v+1||(*end&&*end!='~')||value<0||value>1000000)return -1;
            countFields++;positive|=value>0;
        }
        LjAppend(&b,"%s",clean);
    }
    if(!countFields||b.failed)return -1;
    unsigned char digest[SHA256_DIGEST_LENGTH];SHA256((unsigned char*)normalized,b.len,digest);
    for(int i=0;i<SHA256_DIGEST_LENGTH;i++)snprintf(out->key+i*2,3,"%02x",digest[i]);
    snprintf(out->raw,sizeof out->raw,"%s",clean);out->active=positive;
    return 0;
}
