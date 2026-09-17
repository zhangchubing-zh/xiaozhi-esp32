#include "litecrab/json.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LjToken* alloc_token(LjParser* p, LjToken* t, size_t n) {
    if (p->next >= n)
        return NULL;
    LjToken* x = &t[p->next++];
    x->start = x->end = -1;
    x->size = 0;
    x->parent = -1;
    x->type = LJ_UNDEFINED;
    return x;
}
static void fill_token(LjToken* t, LjType type, int start, int end) {
    t->type = type;
    t->start = start;
    t->end = end;
    t->size = 0;
}
void LjInit(LjParser* p) {
    if (p) {
        p->pos = 0;
        p->next = 0;
        p->super = -1;
    }
}
static int parse_primitive(LjParser* p, const char* js, size_t len, LjToken* ts, size_t count) {
    unsigned start = p->pos;
    for (; p->pos < len; p->pos++) {
        char c = js[p->pos];
        if (c == ':' || c == ',' || c == ']' || c == '}' || isspace((unsigned char) c))
            break;
        if ((unsigned char) c < 32 || c == '"' || c == '\\') {
            p->pos = start;
            return -2;
        }
    }
    if (start == p->pos) {
        p->pos = start;
        return -2;
    }
    LjToken* t = alloc_token(p, ts, count);
    if (!t) {
        p->pos = start;
        return -1;
    }
    fill_token(t, LJ_PRIMITIVE, (int) start, (int) p->pos);
    t->parent = p->super;
    p->pos--;
    return 0;
}
static int parse_string(LjParser* p, const char* js, size_t len, LjToken* ts, size_t count) {
    unsigned start = p->pos++;
    for (; p->pos < len; p->pos++) {
        char c = js[p->pos];
        if (c == '"') {
            LjToken* t = alloc_token(p, ts, count);
            if (!t) {
                p->pos = start;
                return -1;
            }
            fill_token(t, LJ_STRING, (int) start + 1, (int) p->pos);
            t->parent = p->super;
            return 0;
        }
        if (c == '\\') {
            p->pos++;
            if (p->pos >= len)
                return -3;
            c = js[p->pos];
            if (strchr("\"/\\bfnrt", c))
                continue;
            if (c == 'u') {
                for (int i = 0; i < 4; i++) {
                    p->pos++;
                    if (p->pos >= len || !isxdigit((unsigned char) js[p->pos]))
                        return -2;
                }
                continue;
            }
            return -2;
        }
        if ((unsigned char) c < 32)
            return -2;
    }
    p->pos = start;
    return -3;
}
int LjParse(LjParser* p, const char* js, size_t len, LjToken* ts, unsigned count) {
    int r, i;
    if (!p || !js || !ts || !count)
        return -2;
    for (; p->pos < len; p->pos++) {
        char c = js[p->pos];
        LjToken* t;
        switch (c) {
        case '{':
        case '[':
            t = alloc_token(p, ts, count);
            if (!t)
                return -1;
            if (p->super != -1)
                ts[p->super].size++;
            t->type = c == '{' ? LJ_OBJECT : LJ_ARRAY;
            t->start = (int) p->pos;
            t->parent = p->super;
            p->super = (int) p->next - 1;
            break;
        case '}':
        case ']':
            for (i = (int) p->next - 1; i >= 0; i--) {
                if (ts[i].start != -1 && ts[i].end == -1) {
                    if (ts[i].type != (c == '}' ? LJ_OBJECT : LJ_ARRAY))
                        return -2;
                    ts[i].end = (int) p->pos + 1;
                    p->super = ts[i].parent;
                    break;
                }
            }
            if (i < 0)
                return -2;
            break;
        case '"':
            r = parse_string(p, js, len, ts, count);
            if (r < 0)
                return r;
            if (p->super != -1)
                ts[p->super].size++;
            break;
        case '\t':
        case '\r':
        case '\n':
        case ' ':
        case ':':
        case ',':
            break;
        default:
            r = parse_primitive(p, js, len, ts, count);
            if (r < 0)
                return r;
            if (p->super != -1)
                ts[p->super].size++;
            break;
        }
    }
    for (i = (int) p->next - 1; i >= 0; i--)
        if (ts[i].start != -1 && ts[i].end == -1)
            return -3;
    return (int) p->next;
}
int LjTokenEq(const char* j, const LjToken* t, const char* v) {
    size_t n;
    if (!j || !t || !v)
        return 0;
    n = strlen(v);
    return t->type == LJ_STRING && (size_t) (t->end - t->start) == n &&
           !strncmp(j + t->start, v, n);
}
int LjSkip(const LjToken* t, int count, int idx) {
    if (!t || idx < 0 || idx >= count)
        return count;
    int end = t[idx].end;
    idx++;
    while (idx < count && t[idx].start < end)
        idx++;
    return idx;
}
int LjObjectGet(const char* j, const LjToken* t, int count, int obj, const char* key) {
    if (!j || !t || obj < 0 || obj >= count || t[obj].type != LJ_OBJECT)
        return -1;
    int i = obj + 1;
    while (i < count && t[i].start < t[obj].end) {
        int val = i + 1;
        if (val >= count)
            return -1;
        if (LjTokenEq(j, &t[i], key))
            return val;
        i = LjSkip(t, count, val);
    }
    return -1;
}
int LjArrayGet(const LjToken* t, int count, int arr, int index) {
    if (!t || arr < 0 || arr >= count || t[arr].type != LJ_ARRAY || index < 0)
        return -1;
    int i = arr + 1, n = 0;
    while (i < count && t[i].start < t[arr].end) {
        if (n++ == index)
            return i;
        i = LjSkip(t, count, i);
    }
    return -1;
}
static int hex4(const char* s) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        int x = isdigit((unsigned char) c) ? c - '0' : tolower((unsigned char) c) - 'a' + 10;
        if (x < 0 || x > 15)
            return -1;
        v = (v << 4) | x;
    }
    return v;
}
int LjString(const char* j, const LjToken* t, char* out, size_t z) {
    if (!j || !t || !out || !z || t->type != LJ_STRING)
        return -1;
    size_t o = 0;
    for (int i = t->start; i < t->end; i++) {
        unsigned int cp = (unsigned char) j[i];
        int unicode = 0;
        if (cp != '\\') {
            if (o + 1 >= z) {
                out[o] = 0;
                return -1;
            }
            out[o++] = (char) cp;
            continue;
        }
        if (++i >= t->end)
            return -1;
        cp = (unsigned char) j[i];
        if (cp == 'n')
            cp = '\n';
        else if (cp == 'r')
            cp = '\r';
        else if (cp == 't')
            cp = '\t';
        else if (cp == 'b')
            cp = '\b';
        else if (cp == 'f')
            cp = '\f';
        else if (cp == 'u' && i + 4 < t->end) {
            int v = hex4(j + i + 1);
            if (v < 0)
                return -1;
            i += 4;
            cp = (unsigned) v;
            unicode = 1;
            if (cp >= 0xd800 && cp <= 0xdbff && i + 6 < t->end && j[i + 1] == '\\' &&
                j[i + 2] == 'u') {
                int low = hex4(j + i + 3);
                if (low >= 0xdc00 && low <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (unsigned) (low - 0xdc00);
                    i += 6;
                }
            }
        }
        if (!unicode) {
            if (o + 1 >= z) {
                out[o] = 0;
                return -1;
            }
            out[o++] = (char) cp;
            continue;
        }
        unsigned char bytes[4];
        size_t count;
        if (cp < 0x80) {
            bytes[0] = (unsigned char) cp;
            count = 1;
        } else if (cp < 0x800) {
            bytes[0] = (unsigned char) (0xc0 | (cp >> 6));
            bytes[1] = (unsigned char) (0x80 | (cp & 63));
            count = 2;
        } else if (cp < 0x10000) {
            bytes[0] = (unsigned char) (0xe0 | (cp >> 12));
            bytes[1] = (unsigned char) (0x80 | ((cp >> 6) & 63));
            bytes[2] = (unsigned char) (0x80 | (cp & 63));
            count = 3;
        } else {
            bytes[0] = (unsigned char) (0xf0 | (cp >> 18));
            bytes[1] = (unsigned char) (0x80 | ((cp >> 12) & 63));
            bytes[2] = (unsigned char) (0x80 | ((cp >> 6) & 63));
            bytes[3] = (unsigned char) (0x80 | (cp & 63));
            count = 4;
        }
        if (o + count >= z) {
            out[o] = 0;
            return -1;
        }
        memcpy(out + o, bytes, count);
        o += count;
    }
    out[o] = 0;
    return 0;
}
static int primitive_text(const char* j, const LjToken* t, char* b, size_t z) {
    if (!j || !t || t->type != LJ_PRIMITIVE)
        return -1;
    int n = t->end - t->start;
    if (n < 0 || (size_t) n >= z)
        return -1;
    memcpy(b, j + t->start, (size_t) n);
    b[n] = 0;
    return 0;
}
int LjInt64(const char* j, const LjToken* t, int64_t* out) {
    char b[64], *e;
    if (!out || primitive_text(j, t, b, sizeof b))
        return -1;
    errno = 0;
    long long v = strtoll(b, &e, 10);
    if (errno || *e)
        return -1;
    *out = v;
    return 0;
}
int LjDouble(const char* j, const LjToken* t, double* out) {
    char b[96], *e;
    if (!out || primitive_text(j, t, b, sizeof b))
        return -1;
    errno = 0;
    double v = strtod(b, &e);
    if (errno || *e)
        return -1;
    *out = v;
    return 0;
}
int LjBool(const char* j, const LjToken* t, int* out) {
    char b[8];
    if (!out || primitive_text(j, t, b, sizeof b))
        return -1;
    if (!strcmp(b, "true")) {
        *out = 1;
        return 0;
    }
    if (!strcmp(b, "false")) {
        *out = 0;
        return 0;
    }
    return -1;
}
static void strict_ws(const char* s, size_t n, size_t* p) {
    while (*p < n && isspace((unsigned char) s[*p]))
        (*p)++;
}
static int strict_string(const char* s, size_t n, size_t* p) {
    if (*p >= n || s[(*p)++] != '"')
        return -1;
    while (*p < n) {
        unsigned char c = (unsigned char) s[(*p)++];
        if (c == '"')
            return 0;
        if (c < 32)
            return -1;
        if (c == '\\') {
            if (*p >= n)
                return -1;
            c = (unsigned char) s[(*p)++];
            if (strchr("\"\\/bfnrt", c))
                continue;
            if (c != 'u' || *p + 4 > n)
                return -1;
            for (int i = 0; i < 4; i++)
                if (!isxdigit((unsigned char) s[(*p)++]))
                    return -1;
        }
    }
    return -1;
}
static int strict_number(const char* s, size_t n, size_t* p) {
    size_t i = *p;
    if (i < n && s[i] == '-')
        i++;
    if (i >= n)
        return -1;
    if (s[i] == '0')
        i++;
    else {
        if (s[i] < '1' || s[i] > '9')
            return -1;
        while (i < n && isdigit((unsigned char) s[i]))
            i++;
    }
    if (i < n && s[i] == '.') {
        i++;
        size_t start = i;
        while (i < n && isdigit((unsigned char) s[i]))
            i++;
        if (i == start)
            return -1;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-'))
            i++;
        size_t start = i;
        while (i < n && isdigit((unsigned char) s[i]))
            i++;
        if (i == start)
            return -1;
    }
    *p = i;
    return 0;
}
static int strict_value(const char* s, size_t n, size_t* p, int depth) {
    if (depth > 256)
        return -1;
    strict_ws(s, n, p);
    if (*p >= n)
        return -1;
    if (s[*p] == '"')
        return strict_string(s, n, p);
    if (s[*p] == '{') {
        (*p)++;
        strict_ws(s, n, p);
        if (*p < n && s[*p] == '}') {
            (*p)++;
            return 0;
        }
        for (;;) {
            if (strict_string(s, n, p))
                return -1;
            strict_ws(s, n, p);
            if (*p >= n || s[(*p)++] != ':')
                return -1;
            if (strict_value(s, n, p, depth + 1))
                return -1;
            strict_ws(s, n, p);
            if (*p < n && s[*p] == ',') {
                (*p)++;
                strict_ws(s, n, p);
                continue;
            }
            if (*p < n && s[*p] == '}') {
                (*p)++;
                return 0;
            }
            return -1;
        }
    }
    if (s[*p] == '[') {
        (*p)++;
        strict_ws(s, n, p);
        if (*p < n && s[*p] == ']') {
            (*p)++;
            return 0;
        }
        for (;;) {
            if (strict_value(s, n, p, depth + 1))
                return -1;
            strict_ws(s, n, p);
            if (*p < n && s[*p] == ',') {
                (*p)++;
                continue;
            }
            if (*p < n && s[*p] == ']') {
                (*p)++;
                return 0;
            }
            return -1;
        }
    }
    if (n - *p >= 4 && !strncmp(s + *p, "true", 4)) {
        *p += 4;
        return 0;
    }
    if (n - *p >= 5 && !strncmp(s + *p, "false", 5)) {
        *p += 5;
        return 0;
    }
    if (n - *p >= 4 && !strncmp(s + *p, "null", 4)) {
        *p += 4;
        return 0;
    }
    return strict_number(s, n, p);
}
int LjValidate(const char* j, LjType root) {
    if (!j)
        return -1;
    size_t n = strlen(j), p = 0;
    if (strict_value(j, n, &p, 0))
        return -1;
    strict_ws(j, n, &p);
    if (p != n)
        return -1;
    size_t first = 0;
    strict_ws(j, n, &first);
    if (root == LJ_OBJECT && j[first] != '{')
        return -1;
    if (root == LJ_ARRAY && j[first] != '[')
        return -1;
    return 0;
}
void LjBufInit(LjBuf* b, char* d, size_t z) {
    if (!b)
        return;
    b->data = d;
    b->size = z;
    b->len = 0;
    b->failed = !d || !z;
    if (d && z)
        d[0] = 0;
}
int LjAppend(LjBuf* b, const char* fmt, ...) {
    if (!b || b->failed || !fmt)
        return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->data + b->len, b->size - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t) n >= b->size - b->len) {
        b->failed = 1;
        b->data[b->size - 1] = 0;
        return -1;
    }
    b->len += (size_t) n;
    return 0;
}
int LjAppendJsonString(LjBuf* b, const char* v) {
    if (!v)
        v = "";
    if (LjAppend(b, "\"") < 0)
        return -1;
    for (const unsigned char* p = (const unsigned char*) v; *p; p++) {
        switch (*p) {
        case '"':
            if (LjAppend(b, "\\\"") < 0)
                return -1;
            break;
        case '\\':
            if (LjAppend(b, "\\\\") < 0)
                return -1;
            break;
        case '\n':
            if (LjAppend(b, "\\n") < 0)
                return -1;
            break;
        case '\r':
            if (LjAppend(b, "\\r") < 0)
                return -1;
            break;
        case '\t':
            if (LjAppend(b, "\\t") < 0)
                return -1;
            break;
        default:
            if (*p < 32) {
                if (LjAppend(b, "\\u%04x", *p) < 0)
                    return -1;
            } else if (LjAppend(b, "%c", *p) < 0)
                return -1;
        }
    }
    return LjAppend(b, "\"");
}
