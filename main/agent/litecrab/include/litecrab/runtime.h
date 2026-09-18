#ifndef LITECRAB_RUNTIME_H
#define LITECRAB_RUNTIME_H
#include <stddef.h>
#include <stdint.h>

#define CRAB_TOOL_MAX_ARGS 16
#define CRAB_TOOL_NAME_MAX 64
#define CRAB_TOOL_ARG_NAME_MAX 64
#define CRAB_TOOL_CALL_ID_MAX 64
#define CRAB_TOOL_SUMMARY_MAX 4096
#define CRAB_TOOL_CONTENT_MAX 65536
#define CRAB_TOOL_WARNING_MAX 8
#define CRAB_TOOL_WARNING_TEXT_MAX 256
#define CRAB_TOOL_RAW_REF_MAX 256
#define CRAB_TOOL_ARRAY_MAX_ITEMS 32
#define CRAB_TOOL_ARRAY_MAX_ITEM_BYTES 1024
#define CRAB_TOOL_ARRAY_MAX_TOTAL_BYTES 8192
#define CRAB_REGISTRY_MAX_TOOLS 32
#define CRAB_RUNTIME_PATH_MAX 1024

typedef enum {
    CRAB_TOOL_VALUE_NONE = 0,
    CRAB_TOOL_VALUE_STRING,
    CRAB_TOOL_VALUE_INT,
    CRAB_TOOL_VALUE_BOOL,
    CRAB_TOOL_VALUE_STRING_ARRAY
} CrabToolValueType;
typedef struct {
    const char* items[CRAB_TOOL_ARRAY_MAX_ITEMS];
    size_t count;
} CrabToolStringArray;
typedef struct {
    CrabToolValueType type;
    const char* stringValue;
    int64_t intValue;
    int boolValue;
    CrabToolStringArray stringArrayValue;
} CrabToolValue;
typedef struct {
    char name[CRAB_TOOL_ARG_NAME_MAX];
    CrabToolValue value;
} CrabToolArg;
typedef struct {
    char callId[CRAB_TOOL_CALL_ID_MAX];
    char toolName[CRAB_TOOL_NAME_MAX];
    CrabToolArg args[CRAB_TOOL_MAX_ARGS];
    size_t argCount;
} CrabToolCall;

typedef struct {
    int exitCode;
    char *stdoutData, *stderrData;
    size_t stdoutSize, stderrSize;
    int runtimeTruncated;
    int64_t durationMs;
    char rawRef[CRAB_TOOL_RAW_REF_MAX];
} CrabRawResult;
typedef struct {
    int success;
    char summary[CRAB_TOOL_SUMMARY_MAX];
    char content[CRAB_TOOL_CONTENT_MAX];
    size_t contentSize;
    int truncated, omittedItems, rawAvailable;
    char warnings[CRAB_TOOL_WARNING_MAX][CRAB_TOOL_WARNING_TEXT_MAX];
    int warningCount;
} CrabFilteredResult;
typedef struct {
    char callId[64], toolName[64];
    int success, runtimeError, toolExitCode, filterError;
    char summary[CRAB_TOOL_SUMMARY_MAX], content[CRAB_TOOL_CONTENT_MAX];
    int truncated;
    char warnings[CRAB_TOOL_WARNING_MAX][CRAB_TOOL_WARNING_TEXT_MAX];
    int warningCount;
    char rawRef[CRAB_TOOL_RAW_REF_MAX];
} CrabToolResponse;

typedef enum {
    CRAB_RUNTIME_OK = 0,
    CRAB_ERROR_INVALID_ARG = -1001,
    CRAB_ERROR_TOOL_NOT_FOUND = -1002,
    CRAB_ERROR_REGISTRY_FULL = -1003,
    CRAB_ERROR_DUPLICATE_TOOL = -1004,
    CRAB_ERROR_INVALID_TOOL_SPEC = -1005,
    CRAB_ERROR_EXECUTE_FAILED = -1006,
    CRAB_ERROR_FILTER_FAILED = -1007,
    CRAB_ERROR_NO_MEMORY = -1008,
    CRAB_ERROR_PERMISSION = -1009,
    CRAB_ERROR_TOOL_DISABLED = -1010
} CrabRuntimeError;
typedef struct CrabRuntime CrabRuntime;
typedef struct {
    const char *name, *description;
    CrabToolValueType type;
    int required, hasDefault;
    CrabToolValue defaultValue;
    int64_t minInt, maxInt;
    const char* const* enumValues;
    size_t enumCount;
} CrabToolParamSpec;
typedef enum {
    CRAB_TOOL_CONCURRENCY_SHARED_READ = 0,
    CRAB_TOOL_CONCURRENCY_PATH_EXCLUSIVE_WRITE,
    CRAB_TOOL_CONCURRENCY_GLOBAL_EXCLUSIVE
} CrabToolConcurrencyMode;
typedef enum {
    CRAB_TOOL_RISK_LOW = 0,
    CRAB_TOOL_RISK_MEDIUM,
    CRAB_TOOL_RISK_HIGH
} CrabToolRiskLevel;
typedef enum {
    CRAB_TOOL_PERMISSION_FILE_READ = 1 << 0,
    CRAB_TOOL_PERMISSION_FILE_WRITE = 1 << 1,
    CRAB_TOOL_PERMISSION_FILE_SEARCH = 1 << 2,
    CRAB_TOOL_PERMISSION_CLOCK = 1 << 3,
    CRAB_TOOL_PERMISSION_SCHEDULER = 1 << 4,
    CRAB_TOOL_PERMISSION_PROGRAM_EXEC = 1 << 5
} CrabToolPermission;
typedef int (*CrabToolExecuteFn)(CrabRuntime*, const CrabToolCall*, CrabRawResult*);
typedef int (*CrabToolFilterFn)(const CrabToolCall*, const CrabRawResult*, CrabFilteredResult*);
typedef struct {
    const char *name, *description;
    int enabledByDefault, readonly;
    unsigned int permissions;
    CrabToolConcurrencyMode concurrency;
    CrabToolRiskLevel riskLevel;
    const CrabToolParamSpec* params;
    size_t paramCount;
    CrabToolExecuteFn execute;
    CrabToolFilterFn filter;
} CrabToolSpec;
typedef struct {
    const CrabToolSpec* tools;
    size_t toolCount;
} CrabToolCatalogView;
typedef struct {
    CrabToolSpec tools[CRAB_REGISTRY_MAX_TOOLS];
    size_t toolCount;
} CrabToolRegistry;
typedef void (*CrabRuntimeEmitFn)(const char*, void*);
typedef int64_t (*CrabRuntimeNowFn)(void*);
typedef struct {
    const char *workspaceRoot, *rawResultDir;
    int readonlyMode, maxResponseBytes, maxRawBytes, maxWarnings;
    CrabRuntimeEmitFn emitMessage;
    void* emitUserData;
    CrabRuntimeNowFn now;
    void* nowUserData;
} CrabRuntimeConfig;
struct CrabRuntime {
    CrabRuntimeConfig config;
    char workspaceRoot[CRAB_RUNTIME_PATH_MAX], rawResultDir[CRAB_RUNTIME_PATH_MAX];
    CrabToolRegistry registry;
    int started;
};

int CrabRuntimeInit(CrabRuntime*, const CrabRuntimeConfig*);
int CrabRuntimeStart(CrabRuntime*);
void CrabRuntimeStop(CrabRuntime*);
void CrabRuntimeDestroy(CrabRuntime*);
const char* CrabRuntimeGetWorkspaceRoot(const CrabRuntime*);
int CrabRuntimeRegisterBuiltinTools(CrabRuntime*);
int CrabRuntimeGetToolCatalog(const CrabRuntime*, CrabToolCatalogView*);
int CrabRuntimeCallTool(CrabRuntime*, const CrabToolCall*, CrabToolResponse*);
int CrabToolSpecIsDefaultEnabled(const CrabToolSpec*);
void CrabRegistryInit(CrabToolRegistry*);
int CrabRegistryRegister(CrabToolRegistry*, const CrabToolSpec*);
const CrabToolSpec* CrabRegistryFind(const CrabToolRegistry*, const char*);
int CrabRegistryGetCatalog(const CrabToolRegistry*, CrabToolCatalogView*);
const CrabToolArg* CrabToolCallFindArg(const CrabToolCall*, const char*);
const CrabToolParamSpec* CrabToolSpecFindParam(const CrabToolSpec*, const char*);
int CrabToolValidateCall(const CrabToolSpec*, const CrabToolCall*, char*, size_t);
const char* CrabToolArgsGetString(const CrabToolCall*, const char*, const char*);
int64_t CrabToolArgsGetInt(const CrabToolCall*, const char*, int64_t);
int CrabToolArgsGetBool(const CrabToolCall*, const char*, int);
const CrabToolStringArray* CrabToolArgsGetStringArray(const CrabToolCall*, const char*);
void CrabRawResultClear(CrabRawResult*);
int CrabRawResultSetStdout(CrabRawResult*, const char*);
int CrabRawResultSetStderr(CrabRawResult*, const char*);
int CrabFilteredResultSetSummary(CrabFilteredResult*, const char*);
int CrabFilteredResultSetContent(CrabFilteredResult*, const char*);
int CrabFilteredResultAddWarning(CrabFilteredResult*, const char*);
int CrabBasicFilter(const CrabToolCall*, const CrabRawResult*, CrabFilteredResult*);
int CrabBuildToolResponse(const CrabToolSpec*,
                          const CrabToolCall*,
                          const CrabRawResult*,
                          const CrabFilteredResult*,
                          CrabToolResponse*);
int CrabBuildRuntimeError(CrabToolResponse*, const CrabToolCall*, int, const char*);
int CrabBuildFallbackResponse(const CrabToolSpec*,
                              const CrabToolCall*,
                              const CrabRawResult*,
                              CrabToolResponse*);

#endif
