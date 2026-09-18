#ifndef LITECRAB_SESSION_H
#define LITECRAB_SESSION_H

#include "litecrab/working_memory.h"

#include <stddef.h>

#define AGENT_MAX_CONVERSATION_BYTES (24 * 1024)
#define AGENT_MAX_SESSION_ID_LEN 64
#define AGENT_MAX_USER_ID_LEN 64
#define AGENT_MAX_CONVERSATION_MESSAGES 32
#define AGENT_SESSION_CACHE_CAPACITY 32

typedef struct {
    unsigned int slot;
    unsigned int generation;
    char sessionId[AGENT_MAX_SESSION_ID_LEN];
} SessionHandle;

typedef struct {
    char sessionId[AGENT_MAX_SESSION_ID_LEN];
    char userId[AGENT_MAX_USER_ID_LEN];
    unsigned long long revision;
    long long createdTimeMs;
    long long updatedTimeMs;
    int persisted;
} SessionMetadata;

int AgentSessionStoreConfigure(const char* workspaceRoot);
const char* AgentSessionStoreDirectory(void);
int AgentSessionIdValidate(const char* sessionId);
int AgentSessionStateInit(void);
int AgentSessionStateAcquire(const char* sessionId, int create, SessionHandle* out);
int AgentSessionStateUse(const SessionHandle* handle);
void AgentSessionStateRelease(SessionHandle* handle);
int AgentSessionStateFlush(const SessionHandle* handle);
int AgentSessionStateGetMetadata(const SessionHandle* handle, SessionMetadata* out);
void AgentSessionStateMarkDirty(void);

/* Compatibility facade for the current single Run Coordinator. New code should
 * pass SessionHandle explicitly and call Acquire/Use/Release. */
int AgentSessionStateOpen(void);
int AgentSessionStateSelect(const char*, int);
void AgentSessionStateClose(void);
void AgentSessionStateCloseById(const char*);
int AgentSessionStateReleaseById(const char*);
int AgentSessionStateBindUser(const char*);
const char* AgentSessionStateGetUserId(void);
const char* AgentSessionStateGetSessionId(void);
const char* AgentSessionStateGetInternal(void);
const char* AgentSessionStateLoad(void);
int AgentSessionStateSave(const char*);
void AgentSessionStateSetSessionId(const char*);
void AgentSessionStateTrimMessages(void);
WorkingMemory* AgentSessionStateWorkingMemory(void);
void AgentSessionStateSetSkillRoute(const char*, int);
int AgentSessionStateSkillAllowed(const char*);

#endif
