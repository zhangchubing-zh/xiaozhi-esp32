#include "litecrab/json.h"
#include "litecrab/kernel.h"
#include "litecrab/observability.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* OpenSSL EVP ABI declarations keep the build working on minimal WSL images
 * that provide libcrypto but not development headers. */
typedef struct evp_md_ctx_st EVP_MD_CTX;
typedef struct evp_md_st EVP_MD;
extern EVP_MD_CTX* EVP_MD_CTX_new(void);
extern void EVP_MD_CTX_free(EVP_MD_CTX*);
extern const EVP_MD* EVP_sha256(void);
extern int EVP_DigestInit_ex(EVP_MD_CTX*, const EVP_MD*, void*);
extern int EVP_DigestUpdate(EVP_MD_CTX*, const void*, size_t);
extern int EVP_DigestFinal_ex(EVP_MD_CTX*, unsigned char*, unsigned int*);

typedef struct {
    SkillBasicInfo basic;
    SkillExecState exec;
} History;
typedef struct {
    int inUse;
    char sessionId[64];
    SkillStateSuperset active;
    History history[8];
    int historyCount;
} SkillSessionState;
#define SKILL_SESSION_CAPACITY 32
static SkillSessionState skillStates[SKILL_SESSION_CAPACITY];
static SkillSessionState* skill_state(int create) {
    const char* session = AgentSessionStateGetSessionId();
    for (int i = 0; i < SKILL_SESSION_CAPACITY; i++)
        if (skillStates[i].inUse && !strcmp(skillStates[i].sessionId, session))
            return &skillStates[i];
    if (!create)
        return NULL;
    for (int i = 0; i < SKILL_SESSION_CAPACITY; i++)
        if (!skillStates[i].inUse) {
            memset(&skillStates[i], 0, sizeof skillStates[i]);
            skillStates[i].inUse = 1;
            snprintf(skillStates[i].sessionId, sizeof skillStates[i].sessionId, "%s", session);
            return &skillStates[i];
        }
    return NULL;
}
static unsigned long sequence;
static unsigned long interruptionSequence;
static SkillSuspendEntry suspended[16];
static int suspendCount;
static SkillEntry entries[32];
static int entryCount;
static char canonicalSkillRoot[PATH_MAX];
static long long now_ms(void);

#define SKILL_RUN_MAX 64
static SkillRunSnapshot runStore[SKILL_RUN_MAX];
static int runStoreCount;
static pthread_mutex_t runStoreMutex = PTHREAD_MUTEX_INITIALIZER;

const char* SkillRunStatusToString(SkillRunStatus s) {
    static const char* names[] = {"created",
                                  "ready",
                                  "running",
                                  "waiting_input",
                                  "suspended",
                                  "completed",
                                  "failed",
                                  "cancelled"};
    return s >= SKILL_RUN_CREATED && s <= SKILL_RUN_CANCELLED ? names[s] : "unknown";
}
static int status_terminal(SkillRunStatus s) {
    return s == SKILL_RUN_COMPLETED || s == SKILL_RUN_FAILED || s == SKILL_RUN_CANCELLED;
}
static SkillRunSnapshot* run_find_unlocked(const char* session, const char* runId) {
    for (int i = runStoreCount - 1; i >= 0; i--)
        if ((!session || !strcmp(runStore[i].sessionId, session)) &&
            (!runId || !strcmp(runStore[i].skillRunId, runId)))
            return &runStore[i];
    return NULL;
}
static void run_event(const SkillRunSnapshot* r, SkillRunStatus from, const char* reason) {
    char event[768];
    snprintf(event,
             sizeof event,
             "event=skill.run.state_changed session_id=%s skill_run_id=%s skill=%s "
             "from=%s to=%s revision=%lu reason=%s",
             r->sessionId,
             r->skillRunId,
             r->skillName,
             SkillRunStatusToString(from),
             SkillRunStatusToString(r->status),
             r->revision,
             reason ? reason : "unspecified");
    AgentTraceLogMessage("INFO", "skill", event);
}
static void run_create(const char* runId, const char* name, const char* hash) {
    const char* session = AgentSessionStateGetSessionId();
    pthread_mutex_lock(&runStoreMutex);
    if (runStoreCount == SKILL_RUN_MAX) {
        int remove = -1;
        for (int i = 0; i < runStoreCount; i++)
            if (status_terminal(runStore[i].status)) {
                remove = i;
                break;
            }
        if (remove >= 0) {
            memmove(&runStore[remove],
                    &runStore[remove + 1],
                    (size_t) (runStoreCount - remove - 1) * sizeof *runStore);
            runStoreCount--;
        }
    }
    if (runStoreCount < SKILL_RUN_MAX) {
        SkillRunSnapshot* r = &runStore[runStoreCount++];
        memset(r, 0, sizeof *r);
        snprintf(r->skillRunId, sizeof r->skillRunId, "%s", runId);
        snprintf(r->sessionId, sizeof r->sessionId, "%s", session ? session : "");
        snprintf(r->skillName, sizeof r->skillName, "%s", name);
        snprintf(r->definitionHash, sizeof r->definitionHash, "%s", hash ? hash : "");
        r->status = SKILL_RUN_RUNNING;
        r->revision = 1;
        r->createdTimeMs = r->updatedTimeMs = now_ms();
        snprintf(r->reason, sizeof r->reason, "activated");
        run_event(r, SKILL_RUN_CREATED, "activated");
    }
    pthread_mutex_unlock(&runStoreMutex);
}
static void run_transition_current(SkillRunStatus next, const char* reason) {
    const char* session = AgentSessionStateGetSessionId();
    SkillSessionState* state = skill_state(0);
    if (!state)
        return;
    pthread_mutex_lock(&runStoreMutex);
    SkillRunSnapshot* r = run_find_unlocked(session, state->active.basic.skillRunId);
    if (r && !status_terminal(r->status)) {
        SkillRunStatus from = r->status;
        r->status = next;
        r->revision++;
        r->stepCount = state->active.exec.stepCount;
        r->llmCalls = state->active.exec.llmCalls;
        r->updatedTimeMs = now_ms();
        snprintf(r->reason, sizeof r->reason, "%s", reason ? reason : "unspecified");
        run_event(r, from, reason);
    }
    pthread_mutex_unlock(&runStoreMutex);
}
int SkillRunGetSnapshot(const char* session, const char* runId, SkillRunSnapshot* out) {
    if (!session || !runId || !out)
        return -1;
    pthread_mutex_lock(&runStoreMutex);
    SkillRunSnapshot* r = run_find_unlocked(session, runId);
    if (r)
        *out = *r;
    pthread_mutex_unlock(&runStoreMutex);
    return r ? 0 : -1;
}
int SkillRunListPending(const char* session, SkillRunSnapshot* out, int max) {
    if (!session || max < 0)
        return -1;
    int n = 0;
    pthread_mutex_lock(&runStoreMutex);
    int count = runStoreCount > SKILL_RUN_MAX ? SKILL_RUN_MAX : runStoreCount;
    for (int i = 0; i < count && n < max; i++) {
        int index = count - i - 1;
        if (!strcmp(runStore[index].sessionId, session) &&
            (runStore[index].status == SKILL_RUN_WAITING_INPUT ||
             runStore[index].status == SKILL_RUN_SUSPENDED))
            out[n++] = runStore[index];
    }
    pthread_mutex_unlock(&runStoreMutex);
    return n;
}
static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (long long) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void archive_current(void) {
    SkillSessionState* state = skill_state(0);
    if (!state || !state->active.basic.skillName[0])
        return;
    if (state->historyCount == 8) {
        memmove(state->history, state->history + 1, 7 * sizeof *state->history);
        state->historyCount = 7;
    }
    state->history[state->historyCount].basic = state->active.basic;
    state->history[state->historyCount++].exec = state->active.exec;
}
const SkillStateSuperset* SkillSupersetGetActive(void) {
    SkillSessionState* state = skill_state(0);
    return state && state->active.exec.active ? &state->active : NULL;
}
int SkillSupersetActivate(const char* name, const char* scripts) {
    if (!name || !*name)
        return -1;
    SkillSessionState* state = skill_state(1);
    if (!state)
        return -1;
    if (state->active.exec.active)
        archive_current();
    memset(&state->active, 0, sizeof state->active);
    snprintf(state->active.basic.skillName, sizeof state->active.basic.skillName, "%s", name);
    snprintf(state->active.basic.skillRunId,
             sizeof state->active.basic.skillRunId,
             "%s#%03lu",
             name,
             ++sequence);
    snprintf(state->active.basic.scriptsDir,
             sizeof state->active.basic.scriptsDir,
             "%s",
             scripts ? scripts : "");
    state->active.exec.active = 1;
    state->active.exec.phase = SKILL_EXEC_PHASE_RUNNING;
    state->active.exec.createdTimeMs = state->active.exec.lastActiveTimeMs = now_ms();
    const SkillEntry* def = NULL;
    for (int i = 0; i < entryCount; i++)
        if (!strcmp(entries[i].name, name))
            def = &entries[i];
    run_create(state->active.basic.skillRunId, name, def ? def->definitionHash : "");
    return 0;
}
int SkillSupersetFinish(SkillExecPhase phase, const char* outcome) {
    SkillSessionState* state = skill_state(0);
    if (!state || !state->active.exec.active)
        return -1;
    state->active.exec.phase = phase;
    run_transition_current(phase == SKILL_EXEC_PHASE_SUCCESS  ? SKILL_RUN_COMPLETED
                           : phase == SKILL_EXEC_PHASE_FAILED ? SKILL_RUN_FAILED
                                                              : SKILL_RUN_WAITING_INPUT,
                           outcome);
    state->active.exec.active = 0;
    state->active.exec.lastActiveTimeMs = now_ms();
    snprintf(state->active.exec.lastOutcome,
             sizeof state->active.exec.lastOutcome,
             "%s",
             outcome ? outcome : "");
    archive_current();
    memset(&state->active, 0, sizeof state->active);
    state->active.exec.phase = SKILL_EXEC_PHASE_INACTIVE;
    return 0;
}
void SkillSupersetReset(void) {
    memset(skillStates, 0, sizeof skillStates);
}
void SkillSupersetOnSessionClose(void) {
    SkillSessionState* state = skill_state(0);
    if (!state)
        return;
    if (state->active.exec.active) {
        state->active.exec.phase = SKILL_EXEC_PHASE_FAILED;
        run_transition_current(SKILL_RUN_CANCELLED, "session_closed");
    }
    memset(state, 0, sizeof *state);
}
int SkillSupersetUpdateExecPhase(SkillExecPhase p, const char* reason) {
    (void) reason;
    SkillSessionState* state = skill_state(0);
    if (!state || !state->active.exec.active)
        return -1;
    state->active.exec.phase = p;
    state->active.exec.lastActiveTimeMs = now_ms();
    return 0;
}
int SkillSupersetIncrementStep(int* out) {
    SkillSessionState* state = skill_state(0);
    if (!state || !state->active.exec.active)
        return -1;
    state->active.exec.lastActiveTimeMs = now_ms();
    if (out)
        *out = ++state->active.exec.stepCount;
    else
        state->active.exec.stepCount++;
    return 0;
}
int SkillSupersetIncrementLlmCalls(void) {
    SkillSessionState* state = skill_state(0);
    if (!state || !state->active.exec.active)
        return -1;
    state->active.exec.llmCalls++;
    state->active.exec.lastActiveTimeMs = now_ms();
    return 0;
}
const char* SkillExecPhaseToString(SkillExecPhase p) {
    static const char* n[] = {
        "inactive", "running", "waiting_input", "resuming", "success", "failed", "partial"};
    return p >= 0 && p <= SKILL_EXEC_PHASE_PARTIAL ? n[p] : "unknown";
}
int SkillSuspendQueuePush(const SkillStateSuperset* s) {
    if (!s || !s->exec.active)
        return -1;
    if (suspendCount == (int) (sizeof suspended / sizeof suspended[0]))
        return -1;
    suspended[suspendCount].basic = s->basic;
    suspended[suspendCount].exec = s->exec;
    snprintf(suspended[suspendCount].sessionId,
             sizeof suspended[suspendCount].sessionId,
             "%s",
             AgentSessionStateGetSessionId());
    snprintf(suspended[suspendCount].interruptionId,
             sizeof suspended[suspendCount].interruptionId,
             "int-%08lu",
             ++interruptionSequence);
    snprintf(suspended[suspendCount].correlationToken,
             sizeof suspended[suspendCount].correlationToken,
             "skill-%08lu",
             interruptionSequence);
    pthread_mutex_lock(&runStoreMutex);
    SkillRunSnapshot* run = run_find_unlocked(AgentSessionStateGetSessionId(), s->basic.skillRunId);
    if (run) {
        snprintf(run->interruptionId,
                 sizeof run->interruptionId,
                 "%s",
                 suspended[suspendCount].interruptionId);
        snprintf(run->correlationToken,
                 sizeof run->correlationToken,
                 "%s",
                 suspended[suspendCount].correlationToken);
    }
    pthread_mutex_unlock(&runStoreMutex);
    suspendCount++;
    return 0;
}
int SkillSuspendQueuePopByName(const char* name, SkillSuspendEntry* out) {
    if (!name || !out)
        return -1;
    for (int i = suspendCount - 1; i >= 0; i--)
        if (!strcmp(suspended[i].basic.skillName, name) &&
            !strcmp(suspended[i].sessionId, AgentSessionStateGetSessionId())) {
            *out = suspended[i];
            memmove(&suspended[i],
                    &suspended[i + 1],
                    (size_t) (suspendCount - i - 1) * sizeof *suspended);
            suspendCount--;
            return 0;
        }
    return -1;
}
void SkillSuspendQueueClear(void) {
    memset(suspended, 0, sizeof suspended);
    suspendCount = 0;
}
void SkillSuspendQueueClearSession(const char* session) {
    if (!session)
        return;
    for (int i = suspendCount - 1; i >= 0; i--)
        if (!strcmp(suspended[i].sessionId, session)) {
            pthread_mutex_lock(&runStoreMutex);
            SkillRunSnapshot* run = run_find_unlocked(session, suspended[i].basic.skillRunId);
            if (run && !status_terminal(run->status)) {
                SkillRunStatus from = run->status;
                run->status = SKILL_RUN_CANCELLED;
                run->revision++;
                run->updatedTimeMs = now_ms();
                snprintf(run->reason, sizeof run->reason, "session_closed");
                run_event(run, from, "session_closed");
            }
            pthread_mutex_unlock(&runStoreMutex);
            memmove(&suspended[i],
                    &suspended[i + 1],
                    (size_t) (suspendCount - i - 1) * sizeof *suspended);
            suspendCount--;
        }
}
int SkillSuspendQueueGetCount(void) {
    int count = 0;
    const char* session = AgentSessionStateGetSessionId();
    for (int i = 0; i < suspendCount; i++)
        if (!strcmp(suspended[i].sessionId, session))
            count++;
    return count;
}
int SkillSuspendQueueBuildSimpleContext(char* out, size_t z) {
    if (!out || !z)
        return -1;
    size_t off = 0;
    out[0] = 0;
    for (int i = suspendCount - 1; i >= 0; i--) {
        if (strcmp(suspended[i].sessionId, AgentSessionStateGetSessionId()))
            continue;
        int n = snprintf(out + off,
                         z - off,
                         "- %s (steps=%d)\n",
                         suspended[i].basic.skillName,
                         suspended[i].exec.stepCount);
        if (n < 0 || (size_t) n >= z - off)
            return -1;
        off += (size_t) n;
    }
    return 0;
}
int SkillSupersetReactivate(const SkillSuspendEntry* e) {
    if (!e)
        return -1;
    SkillSessionState* state = skill_state(1);
    if (!state)
        return -1;
    memset(&state->active, 0, sizeof state->active);
    state->active.basic = e->basic;
    state->active.exec = e->exec;
    state->active.exec.active = 1;
    state->active.exec.phase = SKILL_EXEC_PHASE_RESUMING;
    state->active.exec.lastActiveTimeMs = now_ms();
    run_transition_current(SKILL_RUN_RUNNING, "resumed");
    WorkingMemorySetStatus(WORKFLOW_RUNNING);
    return 0;
}
static char* trim(char* s) {
    while (isspace((unsigned char) *s))
        s++;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char) e[-1]))
        *--e = 0;
    return s;
}
static void bounded_copy(char* out, size_t z, const char* in) {
    if (!out || !z)
        return;
    size_t n = in ? strlen(in) : 0;
    if (n >= z)
        n = z - 1;
    if (n)
        memcpy(out, in, n);
    out[n] = 0;
}
static void decode_entities(char* s) {
    char* p;
    while ((p = strstr(s, "&#x20;")))
        memmove(p + 1, p + 6, strlen(p + 6) + 1), *p = ' ';
    while ((p = strstr(s, "&amp;")))
        memmove(p + 1, p + 5, strlen(p + 5) + 1), *p = '&';
}
static int content_hash_hex(const unsigned char* data, size_t size, char out[65]) {
    unsigned char digest[32];
    unsigned int digestSize = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
        return -1;
    int ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
             EVP_DigestUpdate(ctx, data, size) == 1 &&
             EVP_DigestFinal_ex(ctx, digest, &digestSize) == 1 && digestSize == sizeof digest;
    EVP_MD_CTX_free(ctx);
    if (!ok)
        return -1;
    for (unsigned int i = 0; i < digestSize; i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[64] = 0;
    return 0;
}
static int parse_skill(const char* path, const char* packageRoot, const char* dir, SkillEntry* e) {
    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n <= 0 || n > 131072) {
        fclose(f);
        return -1;
    }
    char* d = malloc((size_t) n + 1);
    if (!d) {
        fclose(f);
        return -1;
    }
    size_t got = fread(d, 1, (size_t) n, f);
    fclose(f);
    d[got] = 0;
    char hash[65];
    if (content_hash_hex((const unsigned char*) d, got, hash)) {
        free(d);
        return -1;
    }
    size_t normalized = 0;
    for (size_t i = 0; i < got; i++)
        if (d[i] != '\r')
            d[normalized++] = d[i];
    d[normalized] = 0;
    char *firstEnd = strchr(d, '\n'), *start = NULL, *end = NULL;
    if (firstEnd) {
        *firstEnd = 0;
        if (!strcmp(d, "---"))
            start = firstEnd + 1;
        *firstEnd = '\n';
    }
    for (char* cursor = start; cursor && *cursor;) {
        char* lineEnd = strchr(cursor, '\n');
        if (lineEnd)
            *lineEnd = 0;
        int marker = !strcmp(cursor, "---");
        if (lineEnd)
            *lineEnd = '\n';
        if (marker) {
            end = cursor;
            break;
        }
        if (!lineEnd)
            break;
        cursor = lineEnd + 1;
    }
    if (!start || !end) {
        free(d);
        return -1;
    }
    *end = 0;
    memset(e, 0, sizeof *e);
    char* save = NULL;
    char* line = strtok_r(start, "\n", &save);
    size_t po = 0;
    while (line) {
        char *t = trim(line), *colon = strchr(t, ':');
        if (colon) {
            *colon = 0;
            char *k = trim(t), *v = trim(colon + 1);
            size_t vl = strlen(v);
            if (vl > 1 && (*v == '"' || *v == '\'') && v[vl - 1] == *v) {
                v[vl - 1] = 0;
                v++;
            }
            if (!strcmp(k, "name"))
                bounded_copy(e->name, sizeof e->name, v);
            else if (!strcmp(k, "description") || !strcmp(k, "prompt")) {
                bounded_copy(e->prompt + po, sizeof e->prompt - po, v);
                po = strlen(e->prompt);
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    if (!e->name[0])
        bounded_copy(e->name, sizeof e->name, dir);
    decode_entities(e->name);
    decode_entities(e->prompt);
    bounded_copy(e->path, sizeof e->path, path);
    bounded_copy(e->packageRoot, sizeof e->packageRoot, packageRoot);
    bounded_copy(e->definitionHash, sizeof e->definitionHash, hash);
    size_t packageLength = strlen(packageRoot);
    if (packageLength + sizeof "/scripts" > sizeof e->scriptsDir) {
        free(d);
        return -1;
    }
    memcpy(e->scriptsDir, packageRoot, packageLength);
    memcpy(e->scriptsDir + packageLength, "/scripts", sizeof "/scripts");
    free(d);
    return 0;
}
int SkillLoadAllFrom(const char* root) {
    entryCount = 0;
    canonicalSkillRoot[0] = 0;
    if (!root || !*root)
        return 0;
    struct stat rootStat;
    if (lstat(root, &rootStat) || S_ISLNK(rootStat.st_mode) || !S_ISDIR(rootStat.st_mode))
        return 0;
    if (!realpath(root, canonicalSkillRoot))
        return 0;
    DIR* d = opendir(canonicalSkillRoot);
    if (!d)
        return 0;
    char names[32][256];
    int nameCount = 0;
    struct dirent* x;
    while (nameCount < 32 && (x = readdir(d)))
        if (x->d_name[0] != '.')
            bounded_copy(names[nameCount++], sizeof names[0], x->d_name);
    closedir(d);
    for (int i = 0; i < nameCount; i++)
        for (int j = i + 1; j < nameCount; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[256];
                memcpy(t, names[i], 256);
                memcpy(names[i], names[j], 256);
                memcpy(names[j], t, 256);
            }
    for (int i = 0; i < nameCount && entryCount < 32; i++) {
        char package[1024], p[1200];
        struct stat ps, st;
        int pn = snprintf(package, sizeof package, "%s/%s", canonicalSkillRoot, names[i]);
        if (pn < 0 || (size_t) pn >= sizeof package)
            continue;
        snprintf(p, sizeof p, "%s/SKILL.md", package);
        if (!lstat(package, &ps) && S_ISDIR(ps.st_mode) && !S_ISLNK(ps.st_mode) && !lstat(p, &st) &&
            S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode) &&
            !parse_skill(p, package, names[i], &entries[entryCount]))
            entryCount++;
    }
    return entryCount;
}
int SkillLoadAll(void) {
    return 0; /* Relative CWD discovery is intentionally unsupported. */
}
const char* SkillGetCanonicalRoot(void) {
    return canonicalSkillRoot;
}
int SkillGetEntries(SkillEntry* out, int max) {
    if (!out || max < 0)
        return entryCount;
    int n = entryCount < max ? entryCount : max;
    memcpy(out, entries, (size_t) n * sizeof *out);
    return n;
}

static const SkillEntry* find_entry(const char* name) {
    for (int i = 0; i < entryCount; i++)
        if (!strcmp(entries[i].name, name))
            return &entries[i];
    return NULL;
}
static int valid_resource_path(const char* path) {
    if (!path || !*path || path[0] == '/' || strchr(path, '\\') || strchr(path, ':'))
        return 0;
    if (!strcmp(path, "SKILL.md"))
        return 1;
    if (strncmp(path, "references/", 11))
        return 0;
    const char* p = path;
    while (*p) {
        const char* slash = strchr(p, '/');
        size_t n = slash ? (size_t) (slash - p) : strlen(p);
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return 0;
        if (!slash)
            break;
        p = slash + 1;
    }
    return 1;
}
int SkillReadContent(
    const char* name, const char* relative, char** out, size_t* size, int* truncated) {
    if (!name || !out || !valid_resource_path(relative))
        return CRAB_ERROR_INVALID_ARG;
    const SkillEntry* e = find_entry(name);
    if (!e)
        return CRAB_ERROR_EXECUTE_FAILED;
    int rootfd = open(e->packageRoot, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (rootfd < 0)
        return CRAB_ERROR_EXECUTE_FAILED;
    char path[512];
    bounded_copy(path, sizeof path, relative);
    char* save = NULL;
    char* segment = strtok_r(path, "/", &save);
    int current = rootfd, fd = -1;
    while (segment) {
        char* next = strtok_r(NULL, "/", &save);
        fd = openat(current, segment, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (next ? O_DIRECTORY : 0));
        if (current != rootfd)
            close(current);
        if (fd < 0) {
            close(rootfd);
            return CRAB_ERROR_EXECUTE_FAILED;
        }
        current = fd;
        segment = next;
    }
    close(rootfd);
    if (fd < 0)
        return CRAB_ERROR_EXECUTE_FAILED;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        return CRAB_ERROR_PERMISSION;
    }
    size_t cap = st.st_size > 65536 ? 65536 : (size_t) st.st_size;
    char* data = malloc(cap + 1);
    if (!data) {
        close(fd);
        return CRAB_ERROR_NO_MEMORY;
    }
    size_t got = 0;
    while (got < cap) {
        ssize_t n = read(fd, data + got, cap - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t) n;
    }
    close(fd);
    data[got] = 0;
    if (!strcmp(relative, "SKILL.md")) {
        char actual[65];
        if (content_hash_hex((const unsigned char*) data, got, actual) ||
            strcmp(actual, e->definitionHash)) {
            free(data);
            return CRAB_ERROR_PERMISSION;
        }
    }
    *out = data;
    if (size)
        *size = got;
    if (truncated)
        *truncated = (size_t) st.st_size > cap;
    return 0;
}
void SkillHandlerOnSkillStart(AgentTraceScope* scope,
                              const LlmResponse* r,
                              const char* session,
                              const char* request) {
    (void) scope;
    (void) session;
    (void) request;
    if (!r)
        return;
    for (int i = 0; i < r->callCount; i++) {
        if (strcmp(r->calls[i].name, "skill_read"))
            continue;
        char name[128];
        if (!JsonExtractSkillPath(r->calls[i].input, name, sizeof name))
            continue;
        if (!AgentSessionStateSkillAllowed(name))
            continue;
        SkillSuspendEntry old;
        if (!SkillSuspendQueuePopByName(name, &old)) {
            SkillSupersetReactivate(&old);
            LogPrint("[agent] skill_resume name=%s", name);
            continue;
        }
        const SkillStateSuperset* cur = SkillSupersetGetActive();
        if (cur) {
            if (!strcmp(cur->basic.skillName, name))
                continue;
            SkillSuspendQueuePush(cur);
            SkillHandlerSuspend(scope, session, request);
        }
        const SkillEntry* e = find_entry(name);
        const char* scripts = "";
        if (e)
            scripts = e->scriptsDir;
        if (!SkillSupersetActivate(name, scripts))
            LogPrint("[agent] skill_start name=%s", name);
    }
}
void SkillHandlerOnToolResult(AgentTraceScope* scope,
                              const CrabToolCall* c,
                              const char* out,
                              const char* session,
                              const char* request,
                              SkillHandlerTurnState* state) {
    (void) scope;
    (void) session;
    (void) request;
    if (!c || !SkillSupersetGetActive())
        return;
    int code;
    if (!strcmp(c->toolName, "skill_complete")) {
        if (!TextExtractMarkerInt(out, "exit_code:", &code))
            code = -1;
        if (state) {
            state->relatedToolSeen = 1;
            state->hasLastExitCode = 1;
            state->lastExitCode = code;
        }
        SkillHandlerFinish(
            scope, session, request, code ? SKILL_EXEC_PHASE_FAILED : SKILL_EXEC_PHASE_SUCCESS);
        return;
    }
    if (!strcmp(c->toolName, "skill_read")) {
        if (TextExtractMarkerInt(out, "exit_code:", &code) && code)
            SkillHandlerFinish(scope, session, request, SKILL_EXEC_PHASE_FAILED);
        return;
    }
    if (!TextExtractMarkerInt(out, "exit_code:", &code))
        code = -1;
    int step = 0;
    SkillSupersetIncrementStep(&step);
    if (state) {
        state->relatedToolSeen = 1;
        state->hasLastExitCode = 1;
        state->lastExitCode = code;
    }
    LogPrint("[agent] skill_step tool=%s step=%d exitCode=%d", c->toolName, step, code);
}
void SkillHandlerSuspend(AgentTraceScope* scope, const char* session, const char* request) {
    (void) scope;
    (void) session;
    (void) request;
    const SkillStateSuperset* s = SkillSupersetGetActive();
    if (!s)
        return;
    char name[128];
    bounded_copy(name, sizeof name, s->basic.skillName);
    SkillSupersetFinish(SKILL_EXEC_PHASE_PARTIAL, "suspended");
    WorkingMemorySetStatus(WORKFLOW_WAITING_INPUT);
    LogPrint("[agent] skill_end name=%s phase=partial", name);
}
void SkillHandlerFinish(AgentTraceScope* scope,
                        const char* session,
                        const char* request,
                        SkillExecPhase phase) {
    (void) scope;
    (void) session;
    (void) request;
    const SkillStateSuperset* s = SkillSupersetGetActive();
    if (!s)
        return;
    char name[128];
    bounded_copy(name, sizeof name, s->basic.skillName);
    const char* outcome = phase == SKILL_EXEC_PHASE_SUCCESS  ? "ok"
                          : phase == SKILL_EXEC_PHASE_FAILED ? "fail"
                                                             : "partial";
    SkillSupersetFinish(phase, outcome);
    WorkingMemorySetStatus(phase == SKILL_EXEC_PHASE_FAILED    ? WORKFLOW_FAILED
                           : phase == SKILL_EXEC_PHASE_SUCCESS ? WORKFLOW_COMPLETED
                                                               : WORKFLOW_WAITING_INPUT);
    LogPrint("[agent] skill_end name=%s phase=%s", name, SkillExecPhaseToString(phase));
}
char* SkillHandlerExtractFinalText(const char* t, size_t n) {
    if (!t)
        return strdup("");
    char* s = malloc(n + 1);
    if (!s)
        return NULL;
    memcpy(s, t, n);
    s[n] = 0;
    while (n && isspace((unsigned char) s[n - 1]))
        s[--n] = 0;
    WorkingMemoryParseOptions(s);
    return s;
}
int SkillHandlerBuildToolNamesJson(const LlmResponse* r, char* out, size_t z) {
    if (!r || !out || !z)
        return -1;
    LjBuf b;
    LjBufInit(&b, out, z);
    LjAppend(&b, "[");
    for (int i = 0; i < r->callCount; i++) {
        if (i)
            LjAppend(&b, ",");
        LjAppendJsonString(&b, r->calls[i].name);
    }
    LjAppend(&b, "]");
    return b.failed ? -1 : 0;
}
int SkillHandlerBuildToolCallsJson(const LlmResponse* r, char* out, size_t z) {
    if (!r || !out || !z)
        return -1;
    LjBuf b;
    LjBufInit(&b, out, z);
    LjAppend(&b, "[");
    for (int i = 0; i < r->callCount; i++) {
        if (i)
            LjAppend(&b, ",");
        LjAppend(&b, "{\"id\":");
        LjAppendJsonString(&b, r->calls[i].id);
        LjAppend(&b, ",\"type\":\"function\",\"function\":{\"name\":");
        LjAppendJsonString(&b, r->calls[i].name);
        LjAppend(&b, ",\"arguments\":");
        LjAppendJsonString(&b, r->calls[i].input ? r->calls[i].input : "{}");
        LjAppend(&b, "}}");
    }
    LjAppend(&b, "]");
    return b.failed ? -1 : 0;
}
int SkillRouterInit(const CrabToolCatalogView* c) {
    if (!c)
        return -1;
    return 0;
}
static void recent_assistant(const char* history, char* out, size_t z) {
    out[0] = 0;
    if (!history)
        return;
    size_t cap = strlen(history) / 2 + 16;
    LjToken* t = calloc(cap, sizeof *t);
    if (!t)
        return;
    LjParser p;
    LjInit(&p);
    int n = LjParse(&p, history, strlen(history), t, (unsigned) cap);
    if (n > 0 && t[0].type == LJ_ARRAY)
        for (int i = t[0].size - 1; i >= 0; i--) {
            int obj = LjArrayGet(t, n, 0, i);
            int role = LjObjectGet(history, t, n, obj, "role"),
                content = LjObjectGet(history, t, n, obj, "content");
            if (role >= 0 && content >= 0 && LjTokenEq(history, &t[role], "assistant") &&
                t[content].type == LJ_STRING) {
                LjString(history, &t[content], out, z);
                break;
            }
        }
    free(t);
}
typedef struct {
    const char* name;
    const char* description;
    const char* runId;
    const char* interruptionId;
    int score;
    int resume;
} RouteCandidate;
static int contains_ci(const char* text, const char* needle) {
    if (!text || !needle || !*needle)
        return 0;
    size_t n = strlen(needle);
    for (; *text; text++)
        if (!strncasecmp(text, needle, n))
            return 1;
    return 0;
}
static void normalize_evidence_text(const char* input, char* output) {
    const unsigned char* p = (const unsigned char*) (input ? input : "");
    size_t written = 0;
    while (*p) {
        size_t whitespaceBytes = 0;
        if (isspace(*p))
            whitespaceBytes = 1;
        else if (p[0] == 0xe3 && p[1] == 0x80 && p[2] == 0x80)
            whitespaceBytes = 3; /* UTF-8 IDEOGRAPHIC SPACE (U+3000). */
        if (whitespaceBytes) {
            if (written && output[written - 1] != ' ')
                output[written++] = ' ';
            p += whitespaceBytes;
            continue;
        }
        unsigned char c = *p++;
        output[written++] = c >= 'A' && c <= 'Z' ? (char) (c - 'A' + 'a') : (char) c;
    }
    if (written && output[written - 1] == ' ')
        written--;
    output[written] = 0;
}
static int normalized_evidence_matches(const char* user, const char* evidence) {
    if (!user || !evidence || !*evidence)
        return 0;
    char* normalizedUser = malloc(strlen(user) + 1);
    char* normalizedEvidence = malloc(strlen(evidence) + 1);
    if (!normalizedUser || !normalizedEvidence) {
        free(normalizedUser);
        free(normalizedEvidence);
        return 0;
    }
    normalize_evidence_text(user, normalizedUser);
    normalize_evidence_text(evidence, normalizedEvidence);
    int matches = normalizedEvidence[0] && strstr(normalizedUser, normalizedEvidence) != NULL;
    free(normalizedUser);
    free(normalizedEvidence);
    return matches;
}
static int base_intent_query(const char* user) {
    static const char* patterns[] = {"有哪些skill",
                                     "有哪些 skill",
                                     "支持哪些skill",
                                     "支持哪些 skill",
                                     "有哪些技能",
                                     "支持哪些技能",
                                     "skill列表",
                                     "skill 列表",
                                     "哪些指令",
                                     "什么指令",
                                     "支持哪些指令",
                                     "哪些命令",
                                     "什么命令",
                                     "支持哪些命令",
                                     "能做什么",
                                     "可以做什么",
                                     "有什么能力",
                                     "有哪些能力",
                                     "怎么使用",
                                     "如何使用",
                                     "available skills",
                                     "list skills",
                                     "skill list",
                                     "what skills",
                                     "which skills",
                                     "current skills",
                                     "available commands",
                                     "list commands",
                                     "what commands",
                                     "what can you do",
                                     "your capabilities"};
    for (size_t i = 0; i < sizeof patterns / sizeof patterns[0]; i++)
        if (contains_ci(user, patterns[i]))
            return 1;
    if (user) {
        while (isspace((unsigned char) *user))
            user++;
        size_t n = strlen(user);
        while (n && isspace((unsigned char) user[n - 1]))
            n--;
        if ((n == strlen("help") && !strncasecmp(user, "help", n)) ||
            (n == strlen("帮助") && !strncmp(user, "帮助", n)))
            return 1;
    }
    return 0;
}
static int explicit_resume_intent(const char* user) {
    if (!user)
        return 0;
    while (isspace((unsigned char) *user))
        user++;
    static const char* patterns[] = {"继续", "恢复", "接着", "continue", "resume", "proceed"};
    for (size_t i = 0; i < sizeof patterns / sizeof patterns[0]; i++)
        if (contains_ci(user, patterns[i]))
            return 1;
    if (isdigit((unsigned char) *user))
        return 1;
    return !strcasecmp(user, "yes") || !strcasecmp(user, "ok");
}
static int route_score(const SkillEntry* e, const char* user) {
    if (!e || !user)
        return 0;
    if (contains_ci(user, e->name))
        return 100;
    char copy[1024];
    bounded_copy(copy, sizeof copy, user);
    int score = 0;
    char* save = NULL;
    for (char* token = strtok_r(copy, " \t\r\n,.;:!?()[]{}\"'", &save); token;
         token = strtok_r(NULL, " \t\r\n,.;:!?()[]{}\"'", &save))
        if (strlen(token) >= 3 && (contains_ci(e->name, token) || contains_ci(e->prompt, token)))
            score++;
    return score;
}
static void route_set_candidate(SkillRouterResult* r,
                                const RouteCandidate* c,
                                int action,
                                double confidence,
                                const char* reason) {
    r->action = action;
    r->confidence = confidence;
    bounded_copy(r->skillName, sizeof r->skillName, c->name);
    bounded_copy(r->skillRunId, sizeof r->skillRunId, c->runId);
    bounded_copy(r->interruptionId, sizeof r->interruptionId, c->interruptionId);
    bounded_copy(r->reasonCode, sizeof r->reasonCode, reason);
    bounded_copy(r->source, sizeof r->source, "rule");
}
static int parse_resolver_json(const char* json, SkillRouterResult* out) {
    if (!json || LjValidate(json, LJ_OBJECT))
        return -1;
    size_t cap = strlen(json) / 2 + 32;
    LjToken* t = calloc(cap, sizeof *t);
    if (!t)
        return -1;
    LjParser p;
    LjInit(&p);
    int n = LjParse(&p, json, strlen(json), t, (unsigned) cap);
    int s = n > 0 ? LjObjectGet(json, t, n, 0, "selected_skill") : -1;
    int c = n > 0 ? LjObjectGet(json, t, n, 0, "confidence") : -1;
    int rc = n > 0 ? LjObjectGet(json, t, n, 0, "reason_code") : -1;
    int evidence = n > 0 ? LjObjectGet(json, t, n, 0, "evidence") : -1;
    char confidence[32] = "";
    if (s < 0 || c < 0 || rc < 0 || t[rc].type != LJ_STRING || evidence < 0 ||
        t[evidence].type != LJ_STRING) {
        free(t);
        return -1;
    }
    size_t cn = (size_t) (t[c].end - t[c].start);
    if (cn >= sizeof confidence) {
        free(t);
        return -1;
    }
    memcpy(confidence, json + t[c].start, cn);
    confidence[cn] = 0;
    char* end = NULL;
    out->confidence = strtod(confidence, &end);
    if (!end || *end || out->confidence < 0 || out->confidence > 1) {
        free(t);
        return -1;
    }
    if (t[s].type == LJ_STRING) {
        if (LjString(json, &t[s], out->skillName, sizeof out->skillName) ||
            !out->skillName[0]) {
            free(t);
            return -1;
        }
        out->action = SKILL_ROUTE_SELECT;
    } else if (t[s].type == LJ_PRIMITIVE && LjTokenEq(json, &t[s], "null")) {
        out->action = SKILL_ROUTE_BASE;
        out->skillName[0] = 0;
    } else {
        free(t);
        return -1;
    }
    if (LjString(json, &t[rc], out->reasonCode, sizeof out->reasonCode) ||
        LjString(json, &t[evidence], out->evidence, sizeof out->evidence)) {
        free(t);
        return -1;
    }
    bounded_copy(out->source, sizeof out->source, "llm");
    free(t);
    return 0;
}
static int candidate_index(RouteCandidate* candidates, int count, const char* name) {
    for (int i = 0; i < count; i++)
        if (!strcmp(candidates[i].name, name))
            return i;
    return -1;
}
SkillRouterResult SkillRouterRun(const char* history,
                                 const char* user,
                                 const char* replyRun,
                                 const char* replyInterrupt,
                                 const char* correlation,
                                 AgentTraceScope* scope) {
    SkillRouterResult result = {0};
    result.action = SKILL_ROUTE_BASE;
    bounded_copy(result.source, sizeof result.source, "fallback");
    RouteCandidate candidates[8];
    int count = 0;
    const char* session = AgentSessionStateGetSessionId();
    const char* exactValues[3] = {replyInterrupt, replyRun, correlation};
    const char* exactReasons[3] = {"interrupt_id_match", "run_id_match", "correlation_match"};
    int exactIndex = -1;
    const char* exactReason = NULL;
    for (int priority = 0; priority < 3; priority++)
        if (exactValues[priority] && *exactValues[priority]) {
            int matched = -1;
            for (int i = suspendCount - 1; i >= 0; i--) {
                SkillSuspendEntry* e = &suspended[i];
                if (strcmp(e->sessionId, session) || !find_entry(e->basic.skillName))
                    continue;
                const char* actual = priority == 0   ? e->interruptionId
                                     : priority == 1 ? e->basic.skillRunId
                                                     : e->correlationToken;
                if (!strcmp(exactValues[priority], actual)) {
                    matched = i;
                    break;
                }
            }
            if (matched < 0 || (exactIndex >= 0 && exactIndex != matched)) {
                result.action = SKILL_ROUTE_ERROR;
                result.confidence = 0;
                bounded_copy(result.reasonCode,
                             sizeof result.reasonCode,
                             matched < 0 ? "unknown_route_reference"
                                         : "conflicting_route_references");
                bounded_copy(result.question,
                             sizeof result.question,
                             matched < 0 ? "指定的 Skill Run、Interrupt 或 correlation "
                                           "token 不存在或已结束。"
                                          : "指定的 Skill Run、Interrupt 与 correlation "
                                            "token 不属于同一个 Run。");
                bounded_copy(result.source, sizeof result.source, "rule");
                return result;
            }
            exactIndex = matched;
            if (!exactReason)
                exactReason = exactReasons[priority];
        }
    if (exactIndex >= 0) {
        SkillSuspendEntry* e = &suspended[exactIndex];
        RouteCandidate c = {
            e->basic.skillName, "waiting skill", e->basic.skillRunId, e->interruptionId, 0, 1};
        route_set_candidate(&result, &c, SKILL_ROUTE_RESUME, 1.0, exactReason);
        return result;
    }
    for (int i = suspendCount - 1; i >= 0 && count < 8; i--) {
        SkillSuspendEntry* e = &suspended[i];
        if (strcmp(e->sessionId, session))
            continue;
        if (!find_entry(e->basic.skillName))
            continue;
        RouteCandidate c = {
            e->basic.skillName, "waiting skill", e->basic.skillRunId, e->interruptionId, 0, 1};
        candidates[count++] = c;
    }
    if (base_intent_query(user)) {
        result.action = SKILL_ROUTE_BASE;
        result.confidence = 1.0;
        bounded_copy(result.reasonCode, sizeof result.reasonCode, "base_intent_query");
        bounded_copy(result.source, sizeof result.source, "rule");
        return result;
    }
    int resumeIntent = explicit_resume_intent(user);
    if (count == 1 && resumeIntent) {
        route_set_candidate(&result, &candidates[0], SKILL_ROUTE_RESUME, 1.0, "unique_waiting_run");
        return result;
    }
    if (count > 1 && resumeIntent) {
        result.action = SKILL_ROUTE_ERROR;
        result.confidence = 0;
        bounded_copy(result.reasonCode, sizeof result.reasonCode, "ambiguous_waiting_runs");
        bounded_copy(result.question,
                     sizeof result.question,
                     "存在多个待恢复的 Skill Run，请指定 skillRunId 或 interruptionId。");
        bounded_copy(result.source, sizeof result.source, "rule");
        return result;
    }
    int namedMatch = -1, namedMatches = 0;
    for (int i = 0; i < count; i++)
        if (contains_ci(user, candidates[i].name)) {
            namedMatch = i;
            namedMatches++;
        }
    if (namedMatches == 1) {
        route_set_candidate(
            &result, &candidates[namedMatch], SKILL_ROUTE_RESUME, .99, "explicit_waiting_skill");
        return result;
    }
    if (namedMatches > 1) {
        result.action = SKILL_ROUTE_ERROR;
        result.confidence = 0;
        bounded_copy(result.reasonCode, sizeof result.reasonCode, "ambiguous_waiting_skill");
        bounded_copy(result.question,
                     sizeof result.question,
                     "该 Skill 有多个待恢复 Run，请指定 skillRunId 或 interruptionId。");
        bounded_copy(result.source, sizeof result.source, "rule");
        return result;
    }
    if (count && !resumeIntent)
        count = 0;
    if (!count) {
        for (int i = 0; i < entryCount && count < 8; i++) {
            int score = route_score(&entries[i], user);
            candidates[count++] =
                (RouteCandidate){entries[i].name, entries[i].prompt, "", "", score, 0};
        }
        for (int i = 0; i < count; i++)
            for (int j = i + 1; j < count; j++)
                if (candidates[j].score > candidates[i].score) {
                    RouteCandidate t = candidates[i];
                    candidates[i] = candidates[j];
                    candidates[j] = t;
                }
    }
    if (!count) {
        bounded_copy(result.reasonCode, sizeof result.reasonCode, "no_candidates");
        return result;
    }
    char recent[2048], system[6144], messages[4096];
    recent_assistant(history, recent, sizeof recent);
    LjBuf sb;
    LjBufInit(&sb, system, sizeof system);
    LjAppend(&sb,
             "# Skill Resolver\nReturn exactly one JSON object with selected_skill "
             "(candidate name or null), confidence 0..1, reason_code, and evidence. "
             "Select a skill only when the current user explicitly requests the specialized "
             "workflow or outcome described by that skill. Return selected_skill=null when no "
             "candidate clearly applies. General questions, capability/help inquiries, "
             "explanations, and ordinary file operations do not require a skill unless a "
             "candidate explicitly says so. Do not choose the closest candidate merely because "
             "candidates are present. Never select outside candidates. When selecting, evidence "
             "must quote exact text from the current user request that requests the specialized "
             "outcome.\nCandidates:\n");
    for (int i = 0; i < count && i < 5; i++)
        LjAppend(&sb, "- %s: %s\n", candidates[i].name, candidates[i].description);
    LjBuf mb;
    LjBufInit(&mb, messages, sizeof messages);
    LjAppend(&mb, "[{\"role\":\"user\",\"content\":");
    char combined[3072];
    snprintf(
        combined, sizeof combined, "Previous assistant: %s\nUser: %s", recent, user ? user : "");
    LjAppendJsonString(&mb, combined);
    LjAppend(&mb, "}]");
    LlmResponse resp = {0};
    char resolverJson[2048] = "";
    const char* objectStart = NULL;
    const char* objectEnd = NULL;
    /* Follow the same configured response mode as ordinary Agent requests. */
    int llmError = 0;
    if (sb.failed || mb.failed)
        llmError = -200;
    else
        llmError = LlmChatToolsEx(system, messages, "[]", &resp, -1);
    if (scope) {
        const LlmConfig* cfg = LlmGetConfig();
        if (!llmError) {
            scope->totalPromptTokens += resp.promptTokens;
            scope->totalCompletionTokens += resp.completionTokens;
            scope->llmCallCount++;
        }
        AgentTraceLogLlm(scope,
                         cfg ? cfg->model : "",
                         0,
                         llmError ? "error" : "ok",
                         0,
                         "[]",
                         "[]",
                         system,
                         messages,
                         llmError ? "Skill resolver request failed" : resp.text,
                         resp.promptTokens,
                         resp.completionTokens,
                         resp.reasoningTokens,
                         llmError);
    }
    if (!llmError) {
        objectStart = strchr(resp.text, '{');
        objectEnd = strrchr(resp.text, '}');
        if (objectStart && objectEnd && objectEnd >= objectStart) {
            size_t objectSize = (size_t) (objectEnd - objectStart) + 1;
            if (objectSize < sizeof resolverJson) {
                memcpy(resolverJson, objectStart, objectSize);
                resolverJson[objectSize] = 0;
            }
        }
    }
    if (llmError || !resolverJson[0] || parse_resolver_json(resolverJson, &result)) {
        result.action = SKILL_ROUTE_BASE;
        result.confidence = 0;
        result.skillName[0] = 0;
        bounded_copy(result.reasonCode, sizeof result.reasonCode, "resolver_invalid_fallback");
        bounded_copy(result.source, sizeof result.source, "fallback");
    } else if (result.action == SKILL_ROUTE_SELECT) {
        int index = candidate_index(candidates, count, result.skillName);
        if (index < 0 || result.confidence < .80 || !result.evidence[0] ||
            !normalized_evidence_matches(user, result.evidence)) {
            result.action = SKILL_ROUTE_BASE;
            result.confidence = 0;
            result.skillName[0] = 0;
            bounded_copy(result.reasonCode,
                         sizeof result.reasonCode,
                         "selection_validation_failed");
            bounded_copy(result.source, sizeof result.source, "fallback");
        } else if (candidates[index].resume) {
            result.action = SKILL_ROUTE_RESUME;
            bounded_copy(result.skillRunId, sizeof result.skillRunId, candidates[index].runId);
            bounded_copy(result.interruptionId,
                         sizeof result.interruptionId,
                         candidates[index].interruptionId);
        }
    }
    LlmResponseClear(&resp);
    return result;
}
int SkillRouterRecover(const char* name,
                       const char* runId,
                       AgentTraceScope* scope,
                       const char* session,
                       const char* request) {
    (void) scope;
    (void) session;
    (void) request;
    SkillSuspendEntry e;
    int found = -1;
    for (int i = suspendCount - 1; i >= 0; i--)
        if (!strcmp(suspended[i].basic.skillName, name) &&
            !strcmp(suspended[i].sessionId, AgentSessionStateGetSessionId()) &&
            (!runId || !*runId || !strcmp(suspended[i].basic.skillRunId, runId))) {
            e = suspended[i];
            memmove(&suspended[i],
                    &suspended[i + 1],
                    (size_t) (suspendCount - i - 1) * sizeof *suspended);
            suspendCount--;
            found = 0;
            break;
        }
    if (found)
        return -1;
    int rc = SkillSupersetReactivate(&e);
    if (!rc)
        LogPrint("[skill_router] recovered name=%s", name);
    return rc;
}
