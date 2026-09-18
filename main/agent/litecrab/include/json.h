#ifndef LITECRAB_JSON_H
#define LITECRAB_JSON_H
#include <stddef.h>
#include <stdint.h>

typedef enum { LJ_UNDEFINED = 0, LJ_OBJECT, LJ_ARRAY, LJ_STRING, LJ_PRIMITIVE } LjType;
typedef struct {
    LjType type;
    int start, end, size, parent;
} LjToken;
typedef struct {
    unsigned int pos, next;
    int super;
} LjParser;

void LjInit(LjParser* p);
int LjParse(LjParser* p, const char* json, size_t len, LjToken* tokens, unsigned int count);
int LjTokenEq(const char* json, const LjToken* tok, const char* value);
int LjObjectGet(const char* json, const LjToken* tokens, int count, int object, const char* key);
int LjArrayGet(const LjToken* tokens, int count, int array, int index);
int LjSkip(const LjToken* tokens, int count, int index);
int LjString(const char* json, const LjToken* tok, char* out, size_t outSize);
int LjInt64(const char* json, const LjToken* tok, int64_t* out);
int LjDouble(const char* json, const LjToken* tok, double* out);
int LjBool(const char* json, const LjToken* tok, int* out);
int LjValidate(const char* json, LjType rootType);

typedef struct {
    char* data;
    size_t size, len;
    int failed;
} LjBuf;
void LjBufInit(LjBuf* b, char* data, size_t size);
int LjAppend(LjBuf* b, const char* fmt, ...);
int LjAppendJsonString(LjBuf* b, const char* value);

#endif
