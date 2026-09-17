#include "litecrab/kernel.h"
#include "litecrab/runtime.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t monotonic_ms(void);

static int raw_error(CrabRawResult* r, int code, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    r->exitCode = code;
    CrabRawResultSetStderr(r, b);
    return code;
}
static int append(char* out, size_t z, size_t* o, const char* fmt, ...) {
    if (*o >= z)
        return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *o, z - *o, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t) n >= z - *o) {
        *o = z - 1;
        out[*o] = 0;
        return -1;
    }
    *o += (size_t) n;
    return 0;
}
static int normalize_path(const CrabRuntime* r, const char* path, char* out, size_t z) {
    if (!r || !path || !*path || !out || !z)
        return -1;
    char base[PATH_MAX], candidate[PATH_MAX], parent[PATH_MAX], resolvedParent[PATH_MAX];
    if (!realpath(CrabRuntimeGetWorkspaceRoot(r), base))
        return -1;
    int written = path[0] == '/' ? snprintf(candidate, sizeof candidate, "%s", path)
                                 : snprintf(candidate, sizeof candidate, "%s/%s", base, path);
    if (written < 0 || (size_t) written >= sizeof candidate)
        return -1;
    char* slash = strrchr(candidate, '/');
    if (!slash)
        return -1;
    size_t pn = (size_t) (slash - candidate);
    if (!pn)
        pn = 1;
    if (pn >= sizeof parent)
        return -1;
    memcpy(parent, candidate, pn);
    parent[pn] = 0;
    if (!realpath(parent, resolvedParent)) {
        /* New nested paths: lexical traversal check, then use the trusted base. */
        if (strstr(path, ".."))
            return -1;
        written = snprintf(out, z, "%s/%s", base, path);
        return written >= 0 && (size_t) written < z ? 0 : -1;
    }
    size_t bn = strlen(base);
    if (strncmp(resolvedParent, base, bn) || (resolvedParent[bn] && resolvedParent[bn] != '/'))
        return -1;
    return snprintf(out, z, "%s/%s", resolvedParent, slash + 1) >= (int) z ? -1 : 0;
}
static int protected_skill_instruction_path(const char* path) {
    const char* root = SkillGetCanonicalRoot();
    if (!root || !*root || !path || !*path)
        return 0;
    char resolved[PATH_MAX];
    if (!realpath(path, resolved))
        return 0;
    size_t rootLen = strlen(root);
    if (strncmp(resolved, root, rootLen) || resolved[rootLen] != '/')
        return 0;
    const char* skillName = resolved + rootLen + 1;
    const char* tail = strchr(skillName, '/');
    if (!tail || tail == skillName)
        return 0;
    tail++;
    return !strcmp(tail, "SKILL.md") || !strncmp(tail, "references/", 11);
}
static int read_all(const char* p, size_t max, char** out, size_t* sz, int* trunc) {
    FILE* f = fopen(p, "rb");
    if (!f)
        return -errno;
    char* b = malloc(max + 1);
    if (!b) {
        fclose(f);
        return CRAB_ERROR_NO_MEMORY;
    }
    size_t n = fread(b, 1, max, f);
    int extra = fgetc(f);
    if (ferror(f)) {
        int e = -errno;
        free(b);
        fclose(f);
        return e;
    }
    fclose(f);
    b[n] = 0;
    *out = b;
    if (sz)
        *sz = n;
    if (trunc)
        *trunc = extra != EOF;
    return 0;
}
static int make_parents(const char* p) {
    char b[PATH_MAX];
    snprintf(b, sizeof b, "%s", p);
    for (char* s = b + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir(b, 0775) && errno != EEXIST)
                return -errno;
            *s = '/';
        }
    return 0;
}
static int write_all(const char* p, const char* d, size_t n, const char* mode) {
    if (!strcmp(mode, "create") && access(p, F_OK) == 0)
        return -EEXIST;
    FILE* f = fopen(p, !strcmp(mode, "append") ? "ab" : "wb");
    if (!f)
        return -errno;
    size_t w = fwrite(d, 1, n, f);
    int rc = (w == n && !fclose(f)) ? 0 : CRAB_ERROR_EXECUTE_FAILED;
    if (w != n)
        fclose(f);
    return rc;
}
static int do_read(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    char p[PATH_MAX];
    if (normalize_path(r, CrabToolArgsGetString(c, "path", NULL), p, sizeof p))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "invalid or out-of-workspace path");
    if (protected_skill_instruction_path(p))
        return raw_error(raw,
                         CRAB_ERROR_PERMISSION,
                         "skill instructions and references must use skill_read with "
                         "skillPath and fileName");
    size_t max = (size_t) CrabToolArgsGetInt(c, "maxBytes", 65536);
    char* d = NULL;
    size_t n = 0;
    int tr = 0, rc = read_all(p, max, &d, &n, &tr);
    if (rc)
        return raw_error(
            raw, rc, "cannot read %s: %s", p, strerror(rc < 0 && rc > -1000 ? -rc : EIO));
    int64_t start = CrabToolArgsGetInt(c, "startLine", 1),
            lines = CrabToolArgsGetInt(c, "maxLines", 200);
    char* out = calloc(max + 1, 1);
    if (!out) {
        free(d);
        return raw_error(raw, CRAB_ERROR_NO_MEMORY, "out of memory");
    }
    size_t off = 0, pos = 0;
    int64_t ln = 1, em = 0;
    while (pos < n && em < lines) {
        size_t e = pos;
        while (e < n && d[e] != '\n')
            e++;
        if (ln >= start) {
            if (append(
                    out, max + 1, &off, "%lld: %.*s\n", (long long) ln, (int) (e - pos), d + pos)) {
                tr = 1;
                break;
            }
            em++;
        }
        ln++;
        pos = e < n ? e + 1 : e;
    }
    raw->runtimeTruncated = tr;
    raw->exitCode = 0;
    CrabRawResultSetStdout(raw, out);
    free(out);
    free(d);
    return 0;
}
static int do_write(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    char p[PATH_MAX], b[512];
    const char *path = CrabToolArgsGetString(c, "path", NULL),
               *content = CrabToolArgsGetString(c, "content", ""),
               *mode = CrabToolArgsGetString(c, "mode", "overwrite");
    if (normalize_path(r, path, p, sizeof p))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "invalid or out-of-workspace path");
    if (CrabToolArgsGetBool(c, "createDirs", 0)) {
        int x = make_parents(p);
        if (x)
            return raw_error(raw, x, "cannot create parent directories");
    }
    int rc = write_all(p, content, strlen(content), mode);
    if (rc)
        return raw_error(raw, rc, "write failed: %s", strerror(rc < 0 && rc > -1000 ? -rc : EIO));
    snprintf(b, sizeof b, "path: %s\nmode: %s\nbytes: %zu", path, mode, strlen(content));
    CrabRawResultSetStdout(raw, b);
    return 0;
}
static size_t count_occ(const char* s, const char* old) {
    size_t n = 0, l = strlen(old);
    if (!l)
        return 0;
    while ((s = strstr(s, old))) {
        n++;
        s += l;
    }
    return n;
}
static int do_edit(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    char p[PATH_MAX], *d = NULL, b[512];
    const char *path = CrabToolArgsGetString(c, "path", NULL),
               *old = CrabToolArgsGetString(c, "oldText", NULL),
               *nw = CrabToolArgsGetString(c, "newText", "");
    if (!old || !*old)
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "oldText must not be empty");
    if (normalize_path(r, path, p, sizeof p))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "invalid or out-of-workspace path");
    size_t n;
    int tr, rc = read_all(p, 1024 * 1024, &d, &n, &tr);
    if (rc || tr) {
        free(d);
        return raw_error(raw, CRAB_ERROR_EXECUTE_FAILED, "file cannot be read or exceeds 1MB");
    }
    size_t cnt = count_occ(d, old);
    int64_t expected = CrabToolArgsGetInt(c, "expectedOccurrences", 1);
    if (expected < 0 || cnt != (size_t) expected) {
        free(d);
        return raw_error(raw,
                         CRAB_ERROR_EXECUTE_FAILED,
                         "expected %lld occurrences, found %zu",
                         (long long) expected,
                         cnt);
    }
    size_t ol = strlen(old), nl = strlen(nw);
    if (nl > ol && cnt > (SIZE_MAX - n - 1) / (nl - ol)) {
        free(d);
        return raw_error(raw, CRAB_ERROR_NO_MEMORY, "replacement too large");
    }
    size_t z = nl >= ol ? n + 1 + cnt * (nl - ol) : n + 1 - cnt * (ol - nl);
    char* out = malloc(z);
    if (!out) {
        free(d);
        return raw_error(raw, CRAB_ERROR_NO_MEMORY, "out of memory");
    }
    char *o = out, *s = d, *m;
    while ((m = strstr(s, old))) {
        size_t k = (size_t) (m - s);
        memcpy(o, s, k);
        o += k;
        memcpy(o, nw, nl);
        o += nl;
        s = m + ol;
    }
    strcpy(o, s);
    rc = write_all(p, out, strlen(out), "overwrite");
    free(out);
    free(d);
    if (rc)
        return raw_error(raw, rc, "write replacement failed");
    snprintf(b, sizeof b, "path: %s\nreplacements: %zu", path, cnt);
    CrabRawResultSetStdout(raw, b);
    return 0;
}
typedef int (*Visit)(const char*, const char*, int, void*);
typedef struct {
    uint32_t entries;
    uint32_t maxEntries;
    int64_t deadlineMs;
    int limited;
} WalkBudget;
static int walk_depth(
    const char* root, const char* rel, Visit visit, void* u, int depth, WalkBudget* budget) {
    if (depth > 128)
        return -ELOOP;
    char dirp[PATH_MAX];
    int w = !strcmp(rel, ".") ? snprintf(dirp, sizeof dirp, "%s", root)
                              : snprintf(dirp, sizeof dirp, "%s/%s", root, rel);
    if (w < 0 || (size_t) w >= sizeof dirp)
        return -ENAMETOOLONG;
    DIR* d = opendir(dirp);
    if (!d)
        return -errno;
    struct dirent* e;
    int rc = 0;
    while (!rc && (e = readdir(d))) {
        if (monotonic_ms() >= budget->deadlineMs || budget->entries >= budget->maxEntries) {
            budget->limited = 1;
            rc = 1;
            break;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".git") ||
            !strncmp(e->d_name, "build", 5) || !strcmp(e->d_name, "dist") ||
            !strcmp(e->d_name, "node_modules"))
            continue;
        char rr[PATH_MAX], fp[PATH_MAX];
        w = !strcmp(rel, ".") ? snprintf(rr, sizeof rr, "%s", e->d_name)
                              : snprintf(rr, sizeof rr, "%s/%s", rel, e->d_name);
        if (w < 0 || (size_t) w >= sizeof rr)
            continue;
        w = snprintf(fp, sizeof fp, "%s/%s", root, rr);
        if (w < 0 || (size_t) w >= sizeof fp)
            continue;
        struct stat st;
        if (lstat(fp, &st))
            continue;
        budget->entries++;
        int isdir = S_ISDIR(st.st_mode);
        rc = visit(fp, rr, isdir, u);
    }
    rewinddir(d);
    while (!rc && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".git") ||
            !strncmp(e->d_name, "build", 5) || !strcmp(e->d_name, "dist") ||
            !strcmp(e->d_name, "node_modules"))
            continue;
        char rr[PATH_MAX], fp[PATH_MAX];
        w = !strcmp(rel, ".") ? snprintf(rr, sizeof rr, "%s", e->d_name)
                              : snprintf(rr, sizeof rr, "%s/%s", rel, e->d_name);
        if (w < 0 || (size_t) w >= sizeof rr)
            continue;
        w = snprintf(fp, sizeof fp, "%s/%s", root, rr);
        if (w < 0 || (size_t) w >= sizeof fp)
            continue;
        struct stat st;
        if (!lstat(fp, &st) && S_ISDIR(st.st_mode))
            rc = walk_depth(root, rr, visit, u, depth + 1, budget);
    }
    closedir(d);
    return rc;
}
static int walk(const char* root, const char* rel, Visit visit, void* u) {
    WalkBudget budget = {
        .maxEntries = 10000, .deadlineMs = monotonic_ms() + 30000, .limited = 0};
    int rc = walk_depth(root, rel, visit, u, 0, &budget);
    return budget.limited ? -E2BIG : rc;
}
typedef struct {
    const char* pat;
    char* out;
    size_t z, off;
    int max, count, cs, truncated;
    uint64_t scannedBytes;
} GrepCtx;
static int line_match(const char* l, const char* p, int cs) {
    if (cs)
        return strstr(l, p) != NULL;
    size_t n = strlen(p);
    for (; *l; l++)
        if (!strncasecmp(l, p, n))
            return 1;
    return 0;
}
static int valid_utf8_text(const unsigned char* s) {
    while (*s) {
        if (*s < 0x80) {
            s++;
            continue;
        }
        int extra = (*s >= 0xC2 && *s <= 0xDF)   ? 1
                    : (*s >= 0xE0 && *s <= 0xEF) ? 2
                    : (*s >= 0xF0 && *s <= 0xF4) ? 3
                                                 : -1;
        if (extra < 0)
            return 0;
        unsigned char lead = *s++;
        for (int i = 0; i < extra; i++)
            if (s[i] < 0x80 || s[i] > 0xBF)
                return 0;
        if ((lead == 0xE0 && s[0] < 0xA0) || (lead == 0xED && s[0] > 0x9F) ||
            (lead == 0xF0 && s[0] < 0x90) || (lead == 0xF4 && s[0] > 0x8F))
            return 0;
        s += extra;
    }
    return 1;
}
static int grep_visit(const char* fp, const char* rel, int dir, void* u) {
    GrepCtx* x = u;
    if (dir || x->count >= x->max || protected_skill_instruction_path(fp))
        return x->count >= x->max ? 1 : 0;
    struct stat st;
    if (stat(fp, &st) || st.st_size < 0)
        return 0;
    if ((uint64_t) st.st_size > 8U * 1024U * 1024U - x->scannedBytes) {
        x->truncated = 1;
        return 1;
    }
    x->scannedBytes += (uint64_t) st.st_size;
    FILE* f = fopen(fp, "r");
    if (!f)
        return 0;
    unsigned char probe[4096];
    size_t probeSize = fread(probe, 1, sizeof probe, f);
    if (memchr(probe, 0, probeSize)) {
        fclose(f);
        return 0;
    }
    rewind(f);
    char line[4096];
    int no = 0;
    while (x->count < x->max && fgets(line, sizeof line, f)) {
        no++;
        if (valid_utf8_text((const unsigned char*) line) && line_match(line, x->pat, x->cs)) {
            line[strcspn(line, "\r\n")] = 0;
            if (append(x->out, x->z, &x->off, "%s:%d:%s\n", rel, no, line)) {
                x->truncated = 1;
                fclose(f);
                return 1;
            }
            x->count++;
        }
    }
    fclose(f);
    return 0;
}
static int do_grep(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    char root[PATH_MAX];
    if (normalize_path(r, CrabToolArgsGetString(c, "path", "."), root, sizeof root))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "invalid path");
    struct stat st;
    if (stat(root, &st))
        return raw_error(raw, -errno, "search path unavailable");
    char* out = calloc(CRAB_TOOL_CONTENT_MAX, 1);
    if (!out)
        return raw_error(raw, CRAB_ERROR_NO_MEMORY, "out of memory");
    GrepCtx x = {.pat = CrabToolArgsGetString(c, "pattern", NULL),
                 .out = out,
                 .z = CRAB_TOOL_CONTENT_MAX,
                 .max = (int) CrabToolArgsGetInt(c, "maxMatches", 100),
                 .cs = CrabToolArgsGetBool(c, "caseSensitive", 1)};
    if (S_ISDIR(st.st_mode)) {
        int walkResult = walk(root, ".", grep_visit, &x);
        if (walkResult)
            x.truncated = 1;
    }
    else
        grep_visit(root, CrabToolArgsGetString(c, "path", "."), 0, &x);
    raw->runtimeTruncated = x.truncated;
    CrabRawResultSetStdout(raw, out);
    free(out);
    return 0;
}
typedef struct {
    const char* pat;
    char* out;
    size_t z, off;
    int max, count, dirs;
} GlobCtx;
static int glob_visit(const char* fp, const char* rel, int dir, void* u) {
    (void) fp;
    GlobCtx* x = u;
    if (x->count >= x->max)
        return 1;
    if (dir && !x->dirs)
        return 0;
    const char* base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    if (fnmatch(x->pat, rel, 0) && fnmatch(x->pat, base, 0))
        return 0;
    if (append(x->out, x->z, &x->off, "%s\t%s\n", dir ? "dir" : "file", rel))
        return 1;
    x->count++;
    return 0;
}
static int do_glob(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    char root[PATH_MAX];
    if (normalize_path(r, CrabToolArgsGetString(c, "path", "."), root, sizeof root))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "invalid path");
    char* out = calloc(CRAB_TOOL_CONTENT_MAX, 1);
    if (!out)
        return raw_error(raw, CRAB_ERROR_NO_MEMORY, "out of memory");
    GlobCtx x = {CrabToolArgsGetString(c, "pattern", NULL),
                 out,
                 CRAB_TOOL_CONTENT_MAX,
                 0,
                 (int) CrabToolArgsGetInt(c, "maxMatches", 100),
                 0,
                 CrabToolArgsGetBool(c, "includeDirs", 1)};
    int walkResult = walk(root, ".", glob_visit, &x);
    raw->runtimeTruncated = walkResult != 0;
    CrabRawResultSetStdout(raw, out);
    free(out);
    return 0;
}
static int compare_names(const void* left, const void* right) {
    const char* const* a = left;
    const char* const* b = right;
    return strcmp(*a, *b);
}
static int do_ls(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    char normalized[PATH_MAX], dirPath[PATH_MAX], workspace[PATH_MAX];
    const char* requested = CrabToolArgsGetString(c, "path", ".");
    if (normalize_path(r, requested, normalized, sizeof normalized) ||
        !realpath(normalized, dirPath) ||
        !realpath(CrabRuntimeGetWorkspaceRoot(r), workspace))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "invalid or unavailable directory");
    size_t workspaceLen = strlen(workspace);
    if (strncmp(dirPath, workspace, workspaceLen) ||
        (dirPath[workspaceLen] && dirPath[workspaceLen] != '/'))
        return raw_error(raw, CRAB_ERROR_PERMISSION, "directory is outside the workspace");
    struct stat st;
    if (stat(dirPath, &st) || !S_ISDIR(st.st_mode))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "path is not a directory");

    int maxEntries = (int) CrabToolArgsGetInt(c, "maxEntries", 200);
    int showHidden = CrabToolArgsGetBool(c, "showHidden", 0);
    char** names = calloc((size_t) maxEntries + 1, sizeof *names);
    if (!names)
        return raw_error(raw, CRAB_ERROR_NO_MEMORY, "out of memory");
    DIR* dir = opendir(dirPath);
    if (!dir) {
        free(names);
        return raw_error(raw, -errno, "cannot list directory: %s", strerror(errno));
    }
    int count = 0, omitted = 0;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            (!showHidden && entry->d_name[0] == '.'))
            continue;
        if (count >= maxEntries) {
            omitted++;
            continue;
        }
        names[count] = strdup(entry->d_name);
        if (!names[count]) {
            closedir(dir);
            for (int i = 0; i < count; i++)
                free(names[i]);
            free(names);
            return raw_error(raw, CRAB_ERROR_NO_MEMORY, "out of memory");
        }
        count++;
    }
    closedir(dir);
    qsort(names, (size_t) count, sizeof *names, compare_names);

    char* out = calloc(CRAB_TOOL_CONTENT_MAX, 1);
    if (!out) {
        for (int i = 0; i < count; i++)
            free(names[i]);
        free(names);
        return raw_error(raw, CRAB_ERROR_NO_MEMORY, "out of memory");
    }
    size_t off = 0;
    for (int i = 0; i < count; i++) {
        char itemPath[PATH_MAX];
        struct stat itemStat;
        int written = snprintf(itemPath, sizeof itemPath, "%s/%s", dirPath, names[i]);
        const char* kind = "other";
        if (written >= 0 && (size_t) written < sizeof itemPath && !lstat(itemPath, &itemStat))
            kind = S_ISDIR(itemStat.st_mode) ? "dir" : S_ISREG(itemStat.st_mode) ? "file" : "other";
        if (append(out, CRAB_TOOL_CONTENT_MAX, &off, "%s\t%s\n", kind, names[i])) {
            raw->runtimeTruncated = 1;
            omitted += count - i;
            break;
        }
    }
    if (omitted > 0) {
        raw->runtimeTruncated = 1;
        append(out, CRAB_TOOL_CONTENT_MAX, &off, "... %d more entries omitted\n", omitted);
    }
    for (int i = 0; i < count; i++)
        free(names[i]);
    free(names);
    CrabRawResultSetStdout(raw, out);
    free(out);
    return 0;
}
static int do_pwd(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    (void) c;
    char workspace[PATH_MAX];
    if (!realpath(CrabRuntimeGetWorkspaceRoot(r), workspace))
        return raw_error(raw, -errno, "workspace directory is unavailable");
    CrabRawResultSetStdout(raw, workspace);
    return 0;
}
static int64_t monotonic_ms(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) ? 0 : (int64_t) now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static void close_child_descriptors(void) {
#ifdef SYS_close_range
    if (!syscall(SYS_close_range, 3u, ~0u, 0u))
        return;
#endif
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 0 || limit > 65536)
        limit = 65536;
    for (int descriptor = 3; descriptor < limit; descriptor++)
        close(descriptor);
}
static void set_child_limit(int resource, rlim_t value) {
    struct rlimit limit = {.rlim_cur = value, .rlim_max = value};
    if (setrlimit(resource, &limit))
        _exit(126);
}
static int do_exec_program(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    const char *command = CrabToolArgsGetString(c, "command", NULL),
               *requestedPath = CrabToolArgsGetString(c, "path", NULL);
    const CrabToolStringArray* args = CrabToolArgsGetStringArray(c, "args");
    if (!command || !*command || command[0] == '/' || !args)
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "invalid command or arguments");

    char normalized[PATH_MAX], workingPath[PATH_MAX], executable[PATH_MAX], workspace[PATH_MAX];
    if (normalize_path(r, requestedPath, normalized, sizeof normalized) ||
        !realpath(normalized, workingPath) || !realpath(CrabRuntimeGetWorkspaceRoot(r), workspace))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "invalid working path");
    size_t workspaceLen = strlen(workspace);
    if (strncmp(workingPath, workspace, workspaceLen) ||
        (workingPath[workspaceLen] && workingPath[workspaceLen] != '/'))
        return raw_error(raw, CRAB_ERROR_PERMISSION, "working path is outside workspace");
    const char* commandBase = strchr(command, '/') ? workspace : workingPath;
    int written = snprintf(normalized, sizeof normalized, "%s/%s", commandBase, command);
    if (written < 0 || (size_t) written >= sizeof normalized || !realpath(normalized, executable) ||
        strncmp(executable, workspace, workspaceLen) ||
        (executable[workspaceLen] && executable[workspaceLen] != '/'))
        return raw_error(raw, CRAB_ERROR_PERMISSION, "executable is outside workspace");
    struct stat st;
    if (stat(executable, &st) || !S_ISREG(st.st_mode) || access(executable, X_OK))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "command is not an executable file");

    char* argv[CRAB_TOOL_ARRAY_MAX_ITEMS + 2];
    argv[0] = executable;
    for (size_t i = 0; i < args->count; i++)
        argv[i + 1] = (char*) args->items[i];
    argv[args->count + 1] = NULL;

    char userEnv[384], passwordEnv[384];
    const char *user = getenv("LITECRAB_ALARM_USER"), *password = getenv("LITECRAB_ALARM_PASSWORD");
    char* envp[4];
    int envCount = 0;
    envp[envCount++] = "LANG=C.UTF-8";
    if (user && *user) {
        if (snprintf(userEnv, sizeof userEnv, "LITECRAB_ALARM_USER=%s", user) >=
            (int) sizeof userEnv)
            return raw_error(raw, CRAB_ERROR_INVALID_ARG, "alarm user environment is too long");
        envp[envCount++] = userEnv;
    }
    if (password && *password) {
        if (snprintf(passwordEnv, sizeof passwordEnv, "LITECRAB_ALARM_PASSWORD=%s", password) >=
            (int) sizeof passwordEnv)
            return raw_error(raw, CRAB_ERROR_INVALID_ARG, "alarm password environment is too long");
        envp[envCount++] = passwordEnv;
    }
    envp[envCount] = NULL;

    int fd[2];
    if (pipe(fd))
        return raw_error(raw, -errno, "pipe failed");
    pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]); close(fd[1]);
        return raw_error(raw, -errno, "fork failed");
    }
    if (!pid) {
        setpgid(0, 0);
        set_child_limit(RLIMIT_CPU, 15);
        set_child_limit(RLIMIT_AS, 64U * 1024U * 1024U);
        set_child_limit(RLIMIT_NOFILE, 32);
        /* RLIMIT_NPROC is per real user, not per Tool process tree. Applying
         * a tiny value here can prevent legitimate children when LiteCrab
         * shares its board account with other services. Process-tree cleanup
         * is enforced through the dedicated process group and wall deadline. */
        set_child_limit(RLIMIT_FSIZE, 4U * 1024U * 1024U);
        close(fd[0]);
        int nullFd = open("/dev/null", O_RDONLY);
        if (nullFd < 0 || dup2(nullFd, STDIN_FILENO) < 0)
            _exit(126);
        if (nullFd != STDIN_FILENO)
            close(nullFd);
        dup2(fd[1], STDOUT_FILENO);
        dup2(fd[1], STDERR_FILENO);
        close(fd[1]);
        close_child_descriptors();
        if (chdir(workingPath))
            _exit(126);
        execve(executable, argv, envp);
        _exit(127);
    }
    setpgid(pid, pid);
    close(fd[1]);
    fcntl(fd[0], F_SETFL, fcntl(fd[0], F_GETFL, 0) | O_NONBLOCK);
    size_t cap = 256 * 1024, used = 0;
    char* output = malloc(cap + 1);
    if (!output) {
        kill(-pid, SIGKILL); close(fd[0]); waitpid(pid, NULL, 0);
        return raw_error(raw, CRAB_ERROR_NO_MEMORY, "out of memory");
    }
    int timeout = (int) CrabToolArgsGetInt(c, "timeoutMs", 30000), status = 0, done = 0;
    if (timeout < 1 || timeout > 120000)
        timeout = 30000;
    int64_t deadline = monotonic_ms() + timeout;
    while (!done) {
        struct pollfd pollfd = {.fd = fd[0], .events = POLLIN | POLLHUP};
        int remaining = (int) (deadline - monotonic_ms());
        if (remaining <= 0) {
            kill(-pid, SIGTERM);
            poll(&pollfd, 1, 200);
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            raw->runtimeTruncated = 1;
            raw->exitCode = 124;
            done = 1;
            continue;
        }
        poll(&pollfd, 1, remaining > 100 ? 100 : remaining);
        for (;;) {
            ssize_t got = read(fd[0], output + used, cap - used);
            if (got > 0) used += (size_t) got;
            else break;
            if (used == cap) {
                raw->runtimeTruncated = 1;
                kill(-pid, SIGKILL);
                waitpid(pid, &status, 0);
                raw->exitCode = 125;
                done = 1;
                break;
            }
        }
        if (!done && waitpid(pid, &status, WNOHANG) == pid) {
            raw->exitCode = WIFEXITED(status) ? WEXITSTATUS(status) :
                            WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
            done = 1;
        }
    }
    close(fd[0]);
    output[used] = 0;
    CrabRawResultSetStdout(raw, output);
    if (raw->exitCode)
        CrabRawResultSetStderr(raw, raw->exitCode == 124 ? "program timed out" : "program failed");
    free(output);
    return raw->exitCode;
}
static int do_shell(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    const char* s = CrabToolArgsGetString(c, "script", NULL);
    char path[PATH_MAX];
    if (!s || !*s)
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "empty script");
    if (strlen(s) > 4096)
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "script too long");
    if (normalize_path(r, CrabToolArgsGetString(c, "path", "."), path, sizeof path))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "invalid working path");
    int fd[2];
    if (pipe(fd))
        return raw_error(raw, -errno, "pipe failed");
    pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return raw_error(raw, -errno, "fork failed");
    }
    if (!pid) {
        close(fd[0]);
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        close(fd[1]);
        if (chdir(path))
            _exit(126);
        execlp("sh", "sh", "-c", s, (char*) NULL);
        _exit(127);
    }
    close(fd[1]);
    size_t cap = 131072, n = 0;
    char* out = malloc(cap);
    if (!out) {
        close(fd[0]);
        waitpid(pid, NULL, 0);
        return raw_error(raw, CRAB_ERROR_NO_MEMORY, "out of memory");
    }
    ssize_t got;
    while ((got = read(fd[0], out + n, cap - n - 1)) > 0) {
        n += (size_t) got;
        if (cap - n < 4096) {
            if (cap >= 1024 * 1024) {
                raw->runtimeTruncated = 1;
                break;
            }
            size_t nc = cap * 2;
            char* q = realloc(out, nc);
            if (!q)
                break;
            out = q;
            cap = nc;
        }
    }
    close(fd[0]);
    int st;
    waitpid(pid, &st, 0);
    out[n] = 0;
    raw->exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
    CrabRawResultSetStdout(raw, out);
    if (raw->exitCode)
        CrabRawResultSetStderr(raw, "shell command failed");
    free(out);
    return raw->exitCode;
}
static int do_csv(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    char p[PATH_MAX], *d = NULL;
    if (normalize_path(r, CrabToolArgsGetString(c, "path", NULL), p, sizeof p))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "invalid path");
    size_t n;
    int tr, rc = read_all(p, (size_t) CrabToolArgsGetInt(c, "maxBytes", 1024 * 1024), &d, &n, &tr);
    if (rc || tr) {
        free(d);
        return raw_error(raw, CRAB_ERROR_EXECUTE_FAILED, "CSV unreadable or too large");
    }
    int targetRow = (int) CrabToolArgsGetInt(c, "row", 1),
        targetCol = (int) CrabToolArgsGetInt(c, "column", 1), row = 1, col = 1, resolved = 0;
    const char* name = CrabToolArgsGetString(c, "columnName", NULL);
    char field[65536];
    size_t fl = 0;
    int quote = 0, found = 0;
    for (size_t i = 0; i <= n && !found; i++) {
        char ch = i < n ? d[i] : '\n';
        if (quote) {
            if (ch == '"' && i + 1 < n && d[i + 1] == '"') {
                if (fl + 1 < sizeof field)
                    field[fl++] = '"';
                i++;
            } else if (ch == '"')
                quote = 0;
            else if (fl + 1 < sizeof field)
                field[fl++] = ch;
            continue;
        }
        if (ch == '"' && !fl) {
            quote = 1;
            continue;
        }
        if (ch == ',' || ch == '\n' || ch == '\r') {
            field[fl] = 0;
            if (name && row == 1 && !strcmp(field, name))
                resolved = col;
            int wanted = name ? resolved : targetCol;
            if (row == targetRow && wanted && col == wanted)
                found = 1;
            fl = 0;
            if (ch == ',')
                col++;
            else {
                if (ch == '\r' && i + 1 < n && d[i + 1] == '\n')
                    i++;
                row++;
                col = 1;
            }
        } else if (fl + 1 < sizeof field)
            field[fl++] = ch;
    }
    if (quote) {
        free(d);
        return raw_error(raw, CRAB_ERROR_EXECUTE_FAILED, "unclosed CSV quote");
    }
    if (name && !resolved) {
        free(d);
        return raw_error(raw, CRAB_ERROR_EXECUTE_FAILED, "columnName not found");
    }
    if (!found) {
        free(d);
        return raw_error(raw, CRAB_ERROR_EXECUTE_FAILED, "cell not found");
    }
    char* out = malloc(strlen(field) + 512);
    if (!out) {
        free(d);
        return raw_error(raw, CRAB_ERROR_NO_MEMORY, "out of memory");
    }
    snprintf(out,
             strlen(field) + 512,
             "path: %s\nrow: %d\ncolumn: %d\n%svalue: %s",
             CrabToolArgsGetString(c, "path", ""),
             targetRow,
             name ? resolved : targetCol,
             name ? "column_name: selected\n" : "",
             field);
    CrabRawResultSetStdout(raw, out);
    free(out);
    free(d);
    return 0;
}
static int do_skill_read(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    (void) r;
    const char *sp = CrabToolArgsGetString(c, "skillPath", NULL),
               *fn = CrabToolArgsGetString(c, "fileName", "SKILL.md");
    if (!sp || strstr(sp, "..") || sp[0] == '/' || strchr(sp, '\\') || strchr(sp, ':'))
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "invalid skill name");
    if (!AgentSessionStateSkillAllowed(sp))
        return raw_error(raw, CRAB_ERROR_PERMISSION, "skill was not selected by router");
    char* d = NULL;
    size_t n;
    int tr, rc = SkillReadContent(sp, fn, &d, &n, &tr);
    if (rc)
        return raw_error(raw, rc, "skill file not found");
    char *first = strstr(d, "---"), *second = first ? strstr(first + 3, "---") : NULL,
         *body = second ? second + 3 : d;
    while (*body && isspace((unsigned char) *body))
        body++;
    char* end = body + strlen(body);
    while (end > body && isspace((unsigned char) end[-1]))
        *--end = 0;
    raw->runtimeTruncated = tr;
    CrabRawResultSetStdout(raw, body);
    free(d);
    return 0;
}
static int do_skill_complete(CrabRuntime* r, const CrabToolCall* c, CrabRawResult* raw) {
    (void) r;
    if (!SkillSupersetGetActive())
        return raw_error(raw, CRAB_ERROR_INVALID_ARG, "no active skill run");
    const char* summary = CrabToolArgsGetString(c, "summary", "completion requested");
    CrabRawResultSetStdout(raw, summary);
    return 0;
}

#define VSTR(s) {.type = CRAB_TOOL_VALUE_STRING, .stringValue = s}
#define VINT(n) {.type = CRAB_TOOL_VALUE_INT, .intValue = n}
#define VBOOL(n) {.type = CRAB_TOOL_VALUE_BOOL, .boolValue = n}
static const char* write_modes[] = {"create", "overwrite", "append"};
static const CrabToolParamSpec read_p[] = {
    {"path", "file path", CRAB_TOOL_VALUE_STRING, 1, 0, {0}, 0, 0, NULL, 0},
    {"startLine", "first line", CRAB_TOOL_VALUE_INT, 0, 1, VINT(1), 1, 1000000, NULL, 0},
    {"maxLines", "line limit", CRAB_TOOL_VALUE_INT, 0, 1, VINT(200), 1, 100000, NULL, 0},
    {"maxBytes", "byte limit", CRAB_TOOL_VALUE_INT, 0, 1, VINT(65536), 1, 1048576, NULL, 0}};
static const CrabToolParamSpec csv_p[] = {
    {"path", "CSV path", CRAB_TOOL_VALUE_STRING, 1, 0, {0}, 0, 0, NULL, 0},
    {"row", "row", CRAB_TOOL_VALUE_INT, 0, 1, VINT(1), 1, 1000000, NULL, 0},
    {"column", "column", CRAB_TOOL_VALUE_INT, 0, 1, VINT(1), 1, 100000, NULL, 0},
    {"columnName", "column header", CRAB_TOOL_VALUE_STRING, 0, 0, {0}, 0, 0, NULL, 0},
    {"maxBytes", "byte limit", CRAB_TOOL_VALUE_INT, 0, 1, VINT(1048576), 1, 1048576, NULL, 0}};
static const CrabToolParamSpec write_p[] = {
    {"path", "file path", CRAB_TOOL_VALUE_STRING, 1, 0, {0}, 0, 0, NULL, 0},
    {"content", "content", CRAB_TOOL_VALUE_STRING, 0, 1, VSTR(""), 0, 0, NULL, 0},
    {"mode", "write mode", CRAB_TOOL_VALUE_STRING, 0, 1, VSTR("overwrite"), 0, 0, write_modes, 3},
    {"createDirs", "create parents", CRAB_TOOL_VALUE_BOOL, 0, 1, VBOOL(0), 0, 0, NULL, 0}};
static const CrabToolParamSpec edit_p[] = {
    {"path", "file path", CRAB_TOOL_VALUE_STRING, 1, 0, {0}, 0, 0, NULL, 0},
    {"oldText", "exact old text", CRAB_TOOL_VALUE_STRING, 1, 0, {0}, 0, 0, NULL, 0},
    {"newText", "replacement", CRAB_TOOL_VALUE_STRING, 0, 1, VSTR(""), 0, 0, NULL, 0},
    {"expectedOccurrences",
     "expected count",
     CRAB_TOOL_VALUE_INT,
     0,
     1,
     VINT(1),
     0,
     100000,
     NULL,
     0}};
static const CrabToolParamSpec grep_p[] = {
    {"pattern", "substring", CRAB_TOOL_VALUE_STRING, 1, 0, {0}, 0, 0, NULL, 0},
    {"path", "search root", CRAB_TOOL_VALUE_STRING, 0, 1, VSTR("."), 0, 0, NULL, 0},
    {"maxMatches", "match limit", CRAB_TOOL_VALUE_INT, 0, 1, VINT(100), 1, 10000, NULL, 0},
    {"caseSensitive", "case sensitivity", CRAB_TOOL_VALUE_BOOL, 0, 1, VBOOL(1), 0, 0, NULL, 0}};
static const CrabToolParamSpec glob_p[] = {
    {"pattern", "glob pattern", CRAB_TOOL_VALUE_STRING, 1, 0, {0}, 0, 0, NULL, 0},
    {"path", "search root", CRAB_TOOL_VALUE_STRING, 0, 1, VSTR("."), 0, 0, NULL, 0},
    {"maxMatches", "match limit", CRAB_TOOL_VALUE_INT, 0, 1, VINT(100), 1, 10000, NULL, 0},
    {"includeDirs", "include directories", CRAB_TOOL_VALUE_BOOL, 0, 1, VBOOL(1), 0, 0, NULL, 0}};
static const CrabToolParamSpec ls_p[] = {
    {"path", "directory path", CRAB_TOOL_VALUE_STRING, 0, 1, VSTR("."), 0, 0, NULL, 0},
    {"showHidden", "include hidden entries", CRAB_TOOL_VALUE_BOOL, 0, 1, VBOOL(0), 0, 0, NULL, 0},
    {"maxEntries", "entry limit", CRAB_TOOL_VALUE_INT, 0, 1, VINT(200), 1, 10000, NULL, 0}};
static const CrabToolParamSpec exec_p[] = {
    {.name = "command", .description = "executable name or workspace-relative path", .type = CRAB_TOOL_VALUE_STRING, .required = 1},
    {.name = "path", .description = "workspace-relative working directory", .type = CRAB_TOOL_VALUE_STRING, .required = 1},
    {.name = "args", .description = "literal arguments excluding argv[0]", .type = CRAB_TOOL_VALUE_STRING_ARRAY, .required = 1},
    {.name = "timeoutMs", .description = "wall-clock timeout", .type = CRAB_TOOL_VALUE_INT, .hasDefault = 1, .defaultValue = VINT(30000), .minInt = 100, .maxInt = 120000}};
static const CrabToolParamSpec shell_p[] = {
    {"script", "shell script", CRAB_TOOL_VALUE_STRING, 1, 0, {0}, 0, 0, NULL, 0},
    {"path", "working path", CRAB_TOOL_VALUE_STRING, 0, 1, VSTR("."), 0, 0, NULL, 0}};
static const CrabToolParamSpec sr_p[] = {
    {"skillPath", "skill name", CRAB_TOOL_VALUE_STRING, 1, 0, {0}, 0, 0, NULL, 0},
    {"fileName",
     "skill-relative instruction or reference file, for example references/通信异常.md",
     CRAB_TOOL_VALUE_STRING,
     0,
     1,
     VSTR("SKILL.md"),
     0,
     0,
     NULL,
     0}};
static const CrabToolParamSpec complete_p[] = {{"summary",
                                                "completion evidence summary",
                                                CRAB_TOOL_VALUE_STRING,
                                                0,
                                                1,
                                                VSTR("completed"),
                                                0,
                                                0,
                                                NULL,
                                                0}};
#define SPEC(n, d, en, ro, perm, conc, risk, p, fn) \
    {n, d, en, ro, perm, conc, risk, p, sizeof(p) / sizeof((p)[0]), fn, CrabBasicFilter}
static const CrabToolSpec tools[] = {
    SPEC("read",
         "Read a non-skill file with line numbers. Never use this for SKILL.md or files "
         "under a skill's references directory; use skill_read instead.",
         1,
         1,
         CRAB_TOOL_PERMISSION_FILE_READ,
         CRAB_TOOL_CONCURRENCY_SHARED_READ,
         CRAB_TOOL_RISK_LOW,
         read_p,
         do_read),
    SPEC("csv_read",
         "Read a CSV cell.",
         1,
         1,
         CRAB_TOOL_PERMISSION_FILE_READ,
         CRAB_TOOL_CONCURRENCY_SHARED_READ,
         CRAB_TOOL_RISK_LOW,
         csv_p,
         do_csv),
    SPEC("write",
         "Write a file.",
         1,
         0,
         CRAB_TOOL_PERMISSION_FILE_WRITE,
         CRAB_TOOL_CONCURRENCY_PATH_EXCLUSIVE_WRITE,
         CRAB_TOOL_RISK_LOW,
         write_p,
         do_write),
    SPEC("edit",
         "Replace exact text in a file.",
         1,
         0,
         CRAB_TOOL_PERMISSION_FILE_WRITE,
         CRAB_TOOL_CONCURRENCY_PATH_EXCLUSIVE_WRITE,
         CRAB_TOOL_RISK_LOW,
         edit_p,
         do_edit),
    SPEC("grep",
         "Search file contents.",
         1,
         1,
         CRAB_TOOL_PERMISSION_FILE_SEARCH,
         CRAB_TOOL_CONCURRENCY_SHARED_READ,
         CRAB_TOOL_RISK_LOW,
         grep_p,
         do_grep),
    SPEC("glob",
         "Match file names.",
         1,
         1,
         CRAB_TOOL_PERMISSION_FILE_SEARCH,
         CRAB_TOOL_CONCURRENCY_SHARED_READ,
         CRAB_TOOL_RISK_LOW,
         glob_p,
         do_glob),
    SPEC("ls",
         "List the immediate contents of a workspace directory.",
         1,
         1,
         CRAB_TOOL_PERMISSION_FILE_SEARCH,
         CRAB_TOOL_CONCURRENCY_SHARED_READ,
         CRAB_TOOL_RISK_LOW,
         ls_p,
         do_ls),
    {"pwd",
     "Return the agent workspace directory.",
     1,
     1,
     CRAB_TOOL_PERMISSION_FILE_READ,
     CRAB_TOOL_CONCURRENCY_SHARED_READ,
     CRAB_TOOL_RISK_LOW,
     NULL,
     0,
     do_pwd,
     CrabBasicFilter},
    SPEC("exec_program",
         "Execute a workspace program directly without a shell; each args element is one literal argv item.",
         1,
         0,
         CRAB_TOOL_PERMISSION_PROGRAM_EXEC,
         CRAB_TOOL_CONCURRENCY_GLOBAL_EXCLUSIVE,
         CRAB_TOOL_RISK_HIGH,
         exec_p,
         do_exec_program),
    SPEC("shell",
         "Execute a shell command.",
         0,
         0,
         CRAB_TOOL_PERMISSION_SCHEDULER,
         CRAB_TOOL_CONCURRENCY_GLOBAL_EXCLUSIVE,
         CRAB_TOOL_RISK_HIGH,
         shell_p,
         do_shell),
    SPEC("skill_read",
         "Load a skill's SKILL.md or referenced instruction file. For references, pass "
         "the skill name as skillPath and the relative path as fileName.",
         1,
         1,
         CRAB_TOOL_PERMISSION_FILE_READ,
         CRAB_TOOL_CONCURRENCY_SHARED_READ,
         CRAB_TOOL_RISK_LOW,
         sr_p,
         do_skill_read),
    SPEC("skill_complete",
         "Request completion of the active skill after its checks pass.",
         1,
         1,
         0,
         CRAB_TOOL_CONCURRENCY_SHARED_READ,
         CRAB_TOOL_RISK_LOW,
         complete_p,
         do_skill_complete)};
int CrabRuntimeRegisterBuiltinTools(CrabRuntime* r) {
    if (!r)
        return CRAB_ERROR_INVALID_ARG;
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; i++) {
        int rc = CrabRegistryRegister(&r->registry, &tools[i]);
        if (rc)
            return rc;
    }
    return 0;
}
