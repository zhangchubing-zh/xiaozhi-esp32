#include "litecrab/session.h"

#include "litecrab/json.h"
#include "litecrab/observability.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SESSION_FILE_VERSION 1U
#define SESSION_FILE_MAGIC "LCRSESS1"
#define SESSION_STORE_MAX_FILES 128U
#define SESSION_STORE_MAX_BYTES (32U * 1024U * 1024U)
#define SESSION_STORE_TTL_SECONDS (7U * 24U * 60U * 60U)
#define SESSION_FILE_MAX_BYTES (sizeof(SessionDiskHeader) + AGENT_MAX_USER_ID_LEN + \
                                AGENT_MAX_SESSION_ID_LEN + AGENT_MAX_CONVERSATION_BYTES + \
                                sizeof(WorkingMemory) + 128)

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t headerSize;
    uint64_t revision;
    int64_t createdTimeMs;
    int64_t updatedTimeMs;
    uint32_t sessionLen;
    uint32_t userLen;
    uint32_t messagesLen;
    uint32_t memoryLen;
    uint32_t routeEnforced;
    uint32_t routeAllowsSkill;
    uint32_t routedSkillLen;
    uint32_t payloadChecksum;
} SessionDiskHeader;

typedef struct {
    int inUse;
    int dirty;
    unsigned int generation;
    unsigned int references;
    unsigned long long revision;
    long long createdTimeMs;
    long long updatedTimeMs;
    unsigned long long accessSequence;
    char user[AGENT_MAX_USER_ID_LEN];
    char session[AGENT_MAX_SESSION_ID_LEN];
    char messages[AGENT_MAX_CONVERSATION_BYTES];
    WorkingMemory workingMemory;
    int routeEnforced;
    int routeAllowsSkill;
    char routedSkill[128];
} SessionRecord;

static SessionRecord records[AGENT_SESSION_CACHE_CAPACITY] EXT_RAM_BSS_ATTR;
static pthread_mutex_t managerMutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long accessSequence;
static char storeDirectory[PATH_MAX] EXT_RAM_BSS_ATTR = ".crab/sessions";
static _Thread_local SessionHandle currentHandle;

/* Compatibility lifecycle hooks. They will move behind SessionLifecycleHooks
 * when the Skill repository is separated from kernel.c. */
extern void SkillSupersetOnSessionClose(void);
extern void SkillSuspendQueueClearSession(const char* sessionId);

static long long now_ms(void) {
    struct timespec value;
    clock_gettime(CLOCK_REALTIME, &value);
    return (long long) value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static uint32_t checksum_update(uint32_t value, const void* data, size_t size) {
    const unsigned char* bytes = data;
    for (size_t i = 0; i < size; i++) {
        value ^= bytes[i];
        value *= 16777619U;
    }
    return value;
}

static int write_all(int fd, const void* data, size_t size) {
    const unsigned char* cursor = data;
    while (size) {
        ssize_t written = write(fd, cursor, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        cursor += written;
        size -= (size_t) written;
    }
    return 0;
}

static int read_all(int fd, void* data, size_t size) {
    unsigned char* cursor = data;
    while (size) {
        ssize_t got = read(fd, cursor, size);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return -1;
        cursor += got;
        size -= (size_t) got;
    }
    return 0;
}

int AgentSessionIdValidate(const char* id) {
    if (!id || !*id)
        return -1;
    size_t length = strlen(id);
    if (length >= AGENT_MAX_SESSION_ID_LEN)
        return -1;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char) id[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == ':'))
            return -1;
    }
    return 0;
}

static int ensure_directory(const char* path) {
    struct stat status;
    if (!stat(path, &status))
        return S_ISDIR(status.st_mode) ? 0 : -1;
    if (errno != ENOENT || mkdir(path, 0700))
        return -1;
    return 0;
}

int AgentSessionStoreConfigure(const char* workspaceRoot) {
    if (!workspaceRoot || !*workspaceRoot)
        return -1;
    char crab[PATH_MAX];
    int n = snprintf(crab, sizeof crab, "%s/.crab", workspaceRoot);
    if (n < 0 || (size_t) n >= sizeof crab || ensure_directory(crab))
        return -1;
    n = snprintf(storeDirectory, sizeof storeDirectory, "%s/sessions", crab);
    if (n < 0 || (size_t) n >= sizeof storeDirectory || ensure_directory(storeDirectory))
        return -1;
    return 0;
}

const char* AgentSessionStoreDirectory(void) {
    return storeDirectory;
}

static int session_path(const char* id, char* out, size_t size) {
    if (AgentSessionIdValidate(id))
        return -1;
    int n = snprintf(out, size, "%s/%s.lcs", storeDirectory, id);
    return n < 0 || (size_t) n >= size ? -1 : 0;
}
typedef struct {
    char path[PATH_MAX];
    char id[AGENT_MAX_SESSION_ID_LEN];
    off_t size;
    int64_t modifiedNs;
} StoredSession;
static int stored_oldest(const void* left, const void* right) {
    const StoredSession *a = left, *b = right;
    return a->modifiedNs < b->modifiedNs ? -1 : a->modifiedNs > b->modifiedNs ? 1 : 0;
}
static int session_is_referenced(const char* id) {
    for (int i = 0; i < AGENT_SESSION_CACHE_CAPACITY; i++)
        if (records[i].inUse && records[i].references && !strcmp(records[i].session, id))
            return 1;
    return 0;
}
static int enforce_store_quota(const char* destination, size_t incomingBytes) {
    DIR* directory = opendir(storeDirectory);
    if (!directory)
        return -1;
    const size_t candidateCapacity = SESSION_STORE_MAX_FILES + AGENT_SESSION_CACHE_CAPACITY + 64;
    StoredSession* items = calloc(candidateCapacity, sizeof *items);
    if (!items) {
        closedir(directory);
        return -1;
    }
    size_t count = 0, actualCount = 0, bytes = 0, replacedBytes = 0;
    struct dirent* entry;
    time_t now = time(NULL);
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char path[PATH_MAX];
        int n = snprintf(path, sizeof path, "%s/%s", storeDirectory, entry->d_name);
        if (n < 0 || (size_t) n >= sizeof path)
            continue;
        if (strstr(entry->d_name, ".tmp.")) {
            unlink(path);
            continue;
        }
        size_t nameLength = strlen(entry->d_name);
        if (nameLength <= 4 || strcmp(entry->d_name + nameLength - 4, ".lcs"))
            continue;
        struct stat status;
        if (stat(path, &status) || !S_ISREG(status.st_mode))
            continue;
        char id[AGENT_MAX_SESSION_ID_LEN];
        size_t idLength = nameLength - 4;
        if (idLength >= sizeof id)
            idLength = sizeof id - 1;
        memcpy(id, entry->d_name, idLength);
        id[idLength] = 0;
        int expired = now > status.st_mtime &&
                      (unsigned long) (now - status.st_mtime) > SESSION_STORE_TTL_SECONDS;
        if (expired && strcmp(path, destination) && !session_is_referenced(id) && !unlink(path))
            continue;
        if (!strcmp(path, destination))
            replacedBytes = (size_t) status.st_size;
        actualCount++;
        bytes += (size_t) status.st_size;
        if (count < candidateCapacity) {
            StoredSession* item = &items[count++];
            snprintf(item->path, sizeof item->path, "%s", path);
            snprintf(item->id, sizeof item->id, "%s", id);
            item->size = status.st_size;
            item->modifiedNs = (int64_t) status.st_mtim.tv_sec * 1000000000LL +
                               status.st_mtim.tv_nsec;
        } else if (strcmp(path, destination) && !session_is_referenced(id) && !unlink(path)) {
            actualCount--;
            bytes -= (size_t) status.st_size;
        }
    }
    closedir(directory);
    qsort(items, count, sizeof items[0], stored_oldest);
    size_t projectedCount = actualCount + (incomingBytes && !replacedBytes ? 1 : 0);
    size_t projectedBytes = bytes - replacedBytes + incomingBytes;
    for (size_t i = 0; i < count; i++) {
        time_t modifiedSeconds = (time_t) (items[i].modifiedNs / 1000000000LL);
        int expired = now > modifiedSeconds &&
                      (unsigned long) (now - modifiedSeconds) > SESSION_STORE_TTL_SECONDS;
        int over = projectedCount > SESSION_STORE_MAX_FILES ||
                   projectedBytes > SESSION_STORE_MAX_BYTES;
        if ((!expired && !over) || !strcmp(items[i].path, destination) ||
            session_is_referenced(items[i].id))
            continue;
        if (!unlink(items[i].path)) {
            projectedCount--;
            projectedBytes -= (size_t) items[i].size;
        }
    }
    int result = projectedCount <= SESSION_STORE_MAX_FILES && projectedBytes <= SESSION_STORE_MAX_BYTES
                     ? 0
                     : -1;
    free(items);
    return result;
}

static int parse_tokens(const char* json, LjToken** out) {
    if (!json || LjValidate(json, LJ_ARRAY))
        return -1;
    size_t capacity = strlen(json) / 2 + 64;
    LjToken* tokens = calloc(capacity, sizeof *tokens);
    if (!tokens)
        return -1;
    LjParser parser;
    LjInit(&parser);
    int count = LjParse(&parser, json, strlen(json), tokens, (unsigned) capacity);
    if (count < 1 || tokens[0].type != LJ_ARRAY) {
        free(tokens);
        return -1;
    }
    *out = tokens;
    return count;
}

static int copy_trimmed_messages(SessionRecord* record, const char* json) {
    LjToken* tokens = NULL;
    int tokenCount = parse_tokens(json, &tokens);
    if (tokenCount < 1)
        return -1;
    int count = tokens[0].size;
    int start = count > AGENT_MAX_CONVERSATION_MESSAGES
                    ? count - AGENT_MAX_CONVERSATION_MESSAGES
                    : 0;
    while (start < count) {
        int first = LjArrayGet(tokens, tokenCount, 0, start);
        size_t bytes = first >= 0 ? (size_t) (tokens[0].end - 1 - tokens[first].start) + 2 : 2;
        if (bytes < sizeof record->messages)
            break;
        start++;
    }
    if (start >= count)
        strcpy(record->messages, "[]");
    else {
        int first = LjArrayGet(tokens, tokenCount, 0, start);
        size_t bytes = (size_t) (tokens[0].end - 1 - tokens[first].start);
        record->messages[0] = '[';
        memcpy(record->messages + 1, json + tokens[first].start, bytes);
        record->messages[bytes + 1] = ']';
        record->messages[bytes + 2] = 0;
    }
    free(tokens);
    return LjValidate(record->messages, LJ_ARRAY);
}

static int persist_record(SessionRecord* record) {
    if (!record || !record->inUse || AgentSessionIdValidate(record->session))
        return -1;
    if (!strncmp(record->session, "tmp:", 4)) {
        record->dirty = 0;
        return 0;
    }
    char path[PATH_MAX], temporary[PATH_MAX];
    if (session_path(record->session, path, sizeof path))
        return -1;
    int n = snprintf(temporary, sizeof temporary, "%s.tmp.%ld", path, (long) getpid());
    if (n < 0 || (size_t) n >= sizeof temporary)
        return -1;
    size_t sessionLen = strlen(record->session);
    size_t userLen = strlen(record->user);
    size_t messagesLen = strlen(record->messages);
    size_t routedSkillLen = strlen(record->routedSkill);
    size_t encodedSize = sizeof(SessionDiskHeader) + sessionLen + userLen + messagesLen +
                         sizeof record->workingMemory + routedSkillLen;
    if (enforce_store_quota(path, encodedSize))
        return -1;
    uint32_t checksum = 2166136261U;
    checksum = checksum_update(checksum, record->session, sessionLen);
    checksum = checksum_update(checksum, record->user, userLen);
    checksum = checksum_update(checksum, record->messages, messagesLen);
    checksum = checksum_update(checksum, &record->workingMemory, sizeof record->workingMemory);
    checksum = checksum_update(checksum, record->routedSkill, routedSkillLen);
    SessionDiskHeader header = {0};
    memcpy(header.magic, SESSION_FILE_MAGIC, sizeof header.magic);
    header.version = SESSION_FILE_VERSION;
    header.headerSize = sizeof header;
    header.revision = record->revision;
    header.createdTimeMs = record->createdTimeMs;
    header.updatedTimeMs = record->updatedTimeMs;
    header.sessionLen = (uint32_t) sessionLen;
    header.userLen = (uint32_t) userLen;
    header.messagesLen = (uint32_t) messagesLen;
    header.memoryLen = sizeof record->workingMemory;
    header.routeEnforced = record->routeEnforced ? 1U : 0U;
    header.routeAllowsSkill = record->routeAllowsSkill ? 1U : 0U;
    header.routedSkillLen = (uint32_t) routedSkillLen;
    header.payloadChecksum = checksum;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    int rc = write_all(fd, &header, sizeof header) ||
             write_all(fd, record->session, sessionLen) || write_all(fd, record->user, userLen) ||
             write_all(fd, record->messages, messagesLen) ||
             write_all(fd, &record->workingMemory, sizeof record->workingMemory) ||
             write_all(fd, record->routedSkill, routedSkillLen) || fsync(fd);
    if (close(fd))
        rc = -1;
    if (!rc && rename(temporary, path))
        rc = -1;
    if (!rc) {
        int directory = open(storeDirectory, O_RDONLY | O_DIRECTORY);
        if (directory >= 0) {
            if (fsync(directory))
                rc = -1;
            close(directory);
        }
    }
    if (rc)
        unlink(temporary);
    else
        record->dirty = 0;
    return rc;
}

static int load_record(const char* id, SessionRecord* record) {
    char path[PATH_MAX];
    if (session_path(id, path, sizeof path))
        return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? 1 : -1;
    struct stat status;
    SessionDiskHeader header;
    int rc = fstat(fd, &status) || status.st_size < (off_t) sizeof header ||
             status.st_size > (off_t) SESSION_FILE_MAX_BYTES || read_all(fd, &header, sizeof header);
    size_t expected = sizeof header + header.sessionLen + header.userLen + header.messagesLen +
                      header.memoryLen + header.routedSkillLen;
    if (rc || memcmp(header.magic, SESSION_FILE_MAGIC, sizeof header.magic) ||
        header.version != SESSION_FILE_VERSION || header.headerSize != sizeof header ||
        header.sessionLen == 0 || header.sessionLen >= sizeof record->session ||
        header.userLen >= sizeof record->user || header.messagesLen >= sizeof record->messages ||
        header.memoryLen != sizeof record->workingMemory ||
        header.routedSkillLen >= sizeof record->routedSkill || expected != (size_t) status.st_size) {
        close(fd);
        return -1;
    }
    memset(record, 0, sizeof *record);
    rc = read_all(fd, record->session, header.sessionLen) ||
         read_all(fd, record->user, header.userLen) ||
         read_all(fd, record->messages, header.messagesLen) ||
         read_all(fd, &record->workingMemory, sizeof record->workingMemory) ||
         read_all(fd, record->routedSkill, header.routedSkillLen);
    close(fd);
    record->session[header.sessionLen] = 0;
    record->user[header.userLen] = 0;
    record->messages[header.messagesLen] = 0;
    record->routedSkill[header.routedSkillLen] = 0;
    uint32_t checksum = 2166136261U;
    checksum = checksum_update(checksum, record->session, header.sessionLen);
    checksum = checksum_update(checksum, record->user, header.userLen);
    checksum = checksum_update(checksum, record->messages, header.messagesLen);
    checksum = checksum_update(checksum, &record->workingMemory, sizeof record->workingMemory);
    checksum = checksum_update(checksum, record->routedSkill, header.routedSkillLen);
    if (rc || checksum != header.payloadChecksum || strcmp(record->session, id) ||
        LjValidate(record->messages, LJ_ARRAY)) {
        memset(record, 0, sizeof *record);
        return -1;
    }
    record->inUse = 1;
    record->revision = header.revision;
    record->createdTimeMs = header.createdTimeMs;
    record->updatedTimeMs = header.updatedTimeMs;
    record->routeEnforced = header.routeEnforced != 0;
    record->routeAllowsSkill = header.routeAllowsSkill != 0;
    return 0;
}

static int find_record(const char* id) {
    for (int i = 0; i < AGENT_SESSION_CACHE_CAPACITY; i++)
        if (records[i].inUse && !strcmp(records[i].session, id))
            return i;
    return -1;
}

static int select_slot(void) {
    int candidate = -1;
    for (int i = 0; i < AGENT_SESSION_CACHE_CAPACITY; i++) {
        if (!records[i].inUse)
            return i;
        if (!records[i].references &&
            (candidate < 0 || records[i].accessSequence < records[candidate].accessSequence))
            candidate = i;
    }
    if (candidate >= 0 && records[candidate].dirty && persist_record(&records[candidate]))
        return -1;
    return candidate;
}

static SessionRecord* resolve_handle(const SessionHandle* handle) {
    if (!handle || handle->slot >= AGENT_SESSION_CACHE_CAPACITY)
        return NULL;
    SessionRecord* record = &records[handle->slot];
    return record->inUse && record->generation == handle->generation &&
                   !strcmp(record->session, handle->sessionId)
               ? record
               : NULL;
}

int AgentSessionStateInit(void) {
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", storeDirectory);
    char* slash = strrchr(parent, '/');
    if (slash) {
        *slash = 0;
        if (ensure_directory(parent) || ensure_directory(storeDirectory))
            return -1;
    }
    pthread_mutex_lock(&managerMutex);
    for (int i = 0; i < AGENT_SESSION_CACHE_CAPACITY; i++)
        if (records[i].inUse && records[i].dirty)
            persist_record(&records[i]);
    memset(records, 0, sizeof records);
    memset(&currentHandle, 0, sizeof currentHandle);
    accessSequence = 0;
    enforce_store_quota("", 0);
    pthread_mutex_unlock(&managerMutex);
    return 0;
}

int AgentSessionStateAcquire(const char* id, int create, SessionHandle* out) {
    if (!out || AgentSessionIdValidate(id))
        return -1;
    pthread_mutex_lock(&managerMutex);
    int slot = find_record(id);
    if (slot < 0) {
        slot = select_slot();
        if (slot < 0) {
            pthread_mutex_unlock(&managerMutex);
            return -2;
        }
        unsigned int generation = records[slot].generation + 1;
        int loaded = load_record(id, &records[slot]);
        if (loaded == 1 && create) {
            memset(&records[slot], 0, sizeof records[slot]);
            records[slot].inUse = 1;
            records[slot].dirty = 1;
            records[slot].revision = 1;
            records[slot].createdTimeMs = records[slot].updatedTimeMs = now_ms();
            snprintf(records[slot].session, sizeof records[slot].session, "%s", id);
            strcpy(records[slot].messages, "[]");
        } else if (loaded) {
            memset(&records[slot], 0, sizeof records[slot]);
            pthread_mutex_unlock(&managerMutex);
            return -1;
        }
        records[slot].generation = generation ? generation : 1;
    }
    SessionRecord* record = &records[slot];
    record->references++;
    record->accessSequence = ++accessSequence;
    *out = (SessionHandle){.slot = (unsigned int) slot, .generation = record->generation};
    snprintf(out->sessionId, sizeof out->sessionId, "%s", record->session);
    pthread_mutex_unlock(&managerMutex);
    return 0;
}

int AgentSessionStateUse(const SessionHandle* handle) {
    pthread_mutex_lock(&managerMutex);
    SessionRecord* record = resolve_handle(handle);
    if (record)
        currentHandle = *handle;
    pthread_mutex_unlock(&managerMutex);
    return record ? 0 : -1;
}

void AgentSessionStateRelease(SessionHandle* handle) {
    if (!handle)
        return;
    pthread_mutex_lock(&managerMutex);
    SessionRecord* record = resolve_handle(handle);
    if (record) {
        if (record->dirty)
            persist_record(record);
        if (record->references)
            record->references--;
    }
    if (currentHandle.slot == handle->slot && currentHandle.generation == handle->generation)
        memset(&currentHandle, 0, sizeof currentHandle);
    memset(handle, 0, sizeof *handle);
    pthread_mutex_unlock(&managerMutex);
}

int AgentSessionStateFlush(const SessionHandle* handle) {
    pthread_mutex_lock(&managerMutex);
    SessionRecord* record = resolve_handle(handle);
    int rc = !record ? -1 : record->dirty ? persist_record(record) : 0;
    pthread_mutex_unlock(&managerMutex);
    return rc;
}

int AgentSessionStateGetMetadata(const SessionHandle* handle, SessionMetadata* out) {
    if (!out)
        return -1;
    pthread_mutex_lock(&managerMutex);
    SessionRecord* record = resolve_handle(handle);
    if (record) {
        memset(out, 0, sizeof *out);
        snprintf(out->sessionId, sizeof out->sessionId, "%s", record->session);
        snprintf(out->userId, sizeof out->userId, "%s", record->user);
        out->revision = record->revision;
        out->createdTimeMs = record->createdTimeMs;
        out->updatedTimeMs = record->updatedTimeMs;
        out->persisted = !record->dirty;
    }
    pthread_mutex_unlock(&managerMutex);
    return record ? 0 : -1;
}

static SessionRecord* current_record(void) {
    return resolve_handle(&currentHandle);
}

int AgentSessionStateOpen(void) {
    if (current_record())
        return 1;
    char generated[64];
    snprintf(generated, sizeof generated, "local-%lld-%ld", now_ms(), (long) getpid());
    return AgentSessionStateSelect(generated, 1);
}

int AgentSessionStateSelect(const char* id, int create) {
    if (current_record() && !strcmp(currentHandle.sessionId, id))
        return 0;
    SessionHandle old = currentHandle;
    SessionHandle next = {0};
    if (AgentSessionStateAcquire(id, create, &next))
        return -1;
    if (old.generation)
        AgentSessionStateRelease(&old);
    return AgentSessionStateUse(&next);
}

int AgentSessionStateReleaseById(const char* id) {
    SessionHandle handle = {0};
    if (AgentSessionStateAcquire(id, 0, &handle))
        return -1;
    AgentSessionStateRelease(&handle);
    return 0;
}

void AgentSessionStateClose(void) {
    SessionHandle handle = currentHandle;
    if (!handle.generation)
        return;
    AgentSessionStateRelease(&handle);
}

void AgentSessionStateCloseById(const char* id) {
    if (AgentSessionIdValidate(id))
        return;
    SessionHandle previous = currentHandle;
    if (!strcmp(previous.sessionId, id)) {
        SkillSupersetOnSessionClose();
        SkillSuspendQueueClearSession(id);
        SessionHandle selected = currentHandle;
        AgentSessionStateRelease(&selected);
    } else {
        SessionHandle selected = {0};
        if (!AgentSessionStateAcquire(id, 0, &selected)) {
            AgentSessionStateUse(&selected);
            SkillSupersetOnSessionClose();
            SkillSuspendQueueClearSession(id);
            AgentSessionStateRelease(&selected);
            if (previous.generation)
                AgentSessionStateUse(&previous);
        }
    }
    pthread_mutex_lock(&managerMutex);
    int slot = find_record(id);
    if (slot >= 0 && !records[slot].references)
        memset(&records[slot], 0, sizeof records[slot]);
    char path[PATH_MAX];
    if (!session_path(id, path, sizeof path))
        unlink(path);
    pthread_mutex_unlock(&managerMutex);
}

int AgentSessionStateBindUser(const char* user) {
    pthread_mutex_lock(&managerMutex);
    SessionRecord* record = current_record();
    if (!record) {
        pthread_mutex_unlock(&managerMutex);
        return -1;
    }
    const char* value = user && *user ? user : "anonymous";
    if (strlen(value) >= sizeof record->user) {
        pthread_mutex_unlock(&managerMutex);
        return -1;
    }
    if (record->user[0] && strcmp(record->user, value)) {
        pthread_mutex_unlock(&managerMutex);
        return -2;
    }
    if (!record->user[0]) {
        snprintf(record->user, sizeof record->user, "%s", value);
        record->dirty = 1;
        record->updatedTimeMs = now_ms();
        record->revision++;
    }
    pthread_mutex_unlock(&managerMutex);
    return 0;
}

const char* AgentSessionStateGetUserId(void) {
    SessionRecord* record = current_record();
    return record ? record->user : "";
}

const char* AgentSessionStateGetSessionId(void) {
    SessionRecord* record = current_record();
    return record ? record->session : "";
}

const char* AgentSessionStateGetInternal(void) {
    return AgentSessionStateLoad();
}

const char* AgentSessionStateLoad(void) {
    SessionRecord* record = current_record();
    return record ? record->messages : "[]";
}

int AgentSessionStateSave(const char* json) {
    pthread_mutex_lock(&managerMutex);
    SessionRecord* record = current_record();
    if (!record || !json || copy_trimmed_messages(record, json)) {
        pthread_mutex_unlock(&managerMutex);
        return -1;
    }
    record->dirty = 1;
    record->revision++;
    record->updatedTimeMs = now_ms();
    int result = persist_record(record);
    pthread_mutex_unlock(&managerMutex);
    return result;
}

void AgentSessionStateSetSessionId(const char* id) {
    if (id && *id)
        AgentSessionStateSelect(id, 1);
}

void AgentSessionStateTrimMessages(void) {
    SessionRecord* record = current_record();
    if (record)
        copy_trimmed_messages(record, record->messages);
}

WorkingMemory* AgentSessionStateWorkingMemory(void) {
    SessionRecord* record = current_record();
    return record ? &record->workingMemory : NULL;
}

void AgentSessionStateMarkDirty(void) {
    SessionRecord* record = current_record();
    if (!record)
        return;
    record->dirty = 1;
    record->revision++;
    record->updatedTimeMs = now_ms();
}

void AgentSessionStateSetSkillRoute(const char* skill, int allow) {
    SessionRecord* record = current_record();
    if (!record)
        return;
    record->routeEnforced = 1;
    record->routeAllowsSkill = allow && skill && *skill;
    snprintf(record->routedSkill,
             sizeof record->routedSkill,
             "%s",
             record->routeAllowsSkill ? skill : "");
    AgentSessionStateMarkDirty();
}

int AgentSessionStateSkillAllowed(const char* skill) {
    SessionRecord* record = current_record();
    if (!record || !record->routeEnforced)
        return 1;
    return record->routeAllowsSkill && skill && !strcmp(record->routedSkill, skill);
}
