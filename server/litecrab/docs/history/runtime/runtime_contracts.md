# runtime_contracts：契约层数据结构

## 1. 模块职责

`runtime/contract` 定义运行时的**核心数据结构契约**，被 runtime 各子模块及 kernel 的 LLM 工具适配器共同引用。本层只定义类型与枚举，不含实现逻辑。

契约包括：
- `tool_call.h`：工具调用（参数值类型、值联合体、参数、调用结构）。
- `tool_result.h`：工具执行结果（原始结果、过滤结果、最终响应）。
- `tool_schema.h`：工具规格（参数规格、并发/风险/权限、执行/过滤回调）。
- `runtime_error.h`：统一错误码。
- `registry/tool_registry.h`：工具注册表（数据结构部分）。

## 2. tool_call.h —— 工具调用

```c
#define CRAB_TOOL_MAX_ARGS 16
#define CRAB_TOOL_NAME_MAX 64
#define CRAB_TOOL_ARG_NAME_MAX 64
#define CRAB_TOOL_CALL_ID_MAX 64

typedef enum {
    CRAB_TOOL_VALUE_NONE = 0,
    CRAB_TOOL_VALUE_STRING,   /* 字符串 */
    CRAB_TOOL_VALUE_INT,      /* 整数 */
    CRAB_TOOL_VALUE_BOOL      /* 布尔 */
} CrabToolValueType;

typedef struct {
    CrabToolValueType type;
    const char *stringValue;  /* STRING 时有效，不拥有内存 */
    int64_t intValue;         /* INT 时有效 */
    int boolValue;            /* BOOL 时有效 */
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
```

**内存约定**：`CrabToolValue.stringValue` **不拥有内存**，指向外部（LLM 工具适配器解析时可能持有堆副本并提供 `FreeCall`）。

## 3. tool_result.h —— 工具结果（execute -> filtered -> response）

### 3.1 三态模型

```
execute()  -> CrabRawResult   (原始输出，堆分配)
filter()   -> CrabFilteredResult (有界、安全的内容)
组装       -> CrabToolResponse (最终给调用方/LLM 的响应)
```

```c
#define CRAB_TOOL_SUMMARY_MAX 4096
#define CRAB_TOOL_CONTENT_MAX 65536
#define CRAB_TOOL_WARNING_MAX 8
#define CRAB_TOOL_WARNING_TEXT_MAX 256
#define CRAB_TOOL_RAW_REF_MAX 256

/* 原始结果 */
typedef struct {
    int exitCode;               /* 退出码 */
    char *stdoutData;           /* 标准输出（堆分配） */
    char *stderrData;           /* 标准错误（堆分配） */
    size_t stdoutSize, stderrSize;
    int runtimeTruncated;       /* 运行时是否截断 */
    int64_t durationMs;         /* 耗时毫秒 */
    char rawRef[256];           /* 原始结果文件引用 */
} CrabRawResult;

/* 过滤后结果 */
typedef struct {
    int success;
    char summary[4096];
    char content[65536];
    size_t contentSize;
    int truncated;
    int omittedItems;
    int rawAvailable;
    char warnings[8][256];
    int warningCount;
} CrabFilteredResult;

/* 最终响应 */
typedef struct {
    char callId[64];
    char toolName[64];
    int success;
    int runtimeError;           /* 运行时错误码（0 无） */
    int toolExitCode;
    int filterError;
    char summary[4096];
    char content[65536];
    int truncated;
    char warnings[8][256];
    int warningCount;
    char rawRef[256];
} CrabToolResponse;
```

## 4. tool_schema.h —— 工具规格

```c
/* 参数规格 */
typedef struct {
    const char *name;
    const char *description;
    CrabToolValueType type;
    int required;
    int hasDefault;
    CrabToolValue defaultValue;
    int64_t minInt, maxInt;           /* INT 类型范围约束 */
    const char *const *enumValues;    /* STRING 枚举 */
    size_t enumCount;
} CrabToolParamSpec;

/* 并发模式 */
typedef enum {
    CRAB_TOOL_CONCURRENCY_SHARED_READ = 0,   /* 共享读 */
    CRAB_TOOL_CONCURRENCY_PATH_EXCLUSIVE_WRITE, /* 路径排他写 */
    CRAB_TOOL_CONCURRENCY_GLOBAL_EXCLUSIVE      /* 全局排他 */
} CrabToolConcurrencyMode;

/* 风险等级 */
typedef enum { CRAB_TOOL_RISK_LOW=0, CRAB_TOOL_RISK_MEDIUM, CRAB_TOOL_RISK_HIGH } CrabToolRiskLevel;

/* 权限标志位 */
typedef enum {
    CRAB_TOOL_PERMISSION_FILE_READ   = 1 << 0,
    CRAB_TOOL_PERMISSION_FILE_WRITE  = 1 << 1,
    CRAB_TOOL_PERMISSION_FILE_SEARCH = 1 << 2,
    CRAB_TOOL_PERMISSION_CLOCK       = 1 << 3,
    CRAB_TOOL_PERMISSION_SCHEDULER   = 1 << 4
} CrabToolPermission;

/* 执行/过滤回调 */
typedef int (*CrabToolExecuteFn)(CrabRuntime *runtime, const CrabToolCall *call, CrabRawResult *raw);
typedef int (*CrabToolFilterFn)(const CrabToolCall *call, const CrabRawResult *raw, CrabFilteredResult *filtered);

/* 完整工具规格 */
typedef struct {
    const char *name;
    const char *description;
    int enabledByDefault;
    int readonly;
    unsigned int permissions;
    CrabToolConcurrencyMode concurrency;
    CrabToolRiskLevel riskLevel;
    const CrabToolParamSpec *params;
    size_t paramCount;
    CrabToolExecuteFn execute;
    CrabToolFilterFn filter;
} CrabToolSpec;

/* 工具目录只读视图（供 LLM tools JSON 渲染） */
typedef struct {
    const CrabToolSpec *tools;
    size_t toolCount;
} CrabToolCatalogView;
```

**核心扩展点**：每个工具 = 一组元数据 + `execute`（执行）+ `filter`（过滤）两个回调。新增工具只需实现这两个回调并注册。

## 5. runtime_error.h —— 错误码

```c
typedef enum {
    CRAB_RUNTIME_OK             = 0,
    CRAB_ERROR_INVALID_ARG      = -1001,
    CRAB_ERROR_TOOL_NOT_FOUND   = -1002,
    CRAB_ERROR_REGISTRY_FULL    = -1003,
    CRAB_ERROR_DUPLICATE_TOOL   = -1004,
    CRAB_ERROR_INVALID_TOOL_SPEC= -1005,
    CRAB_ERROR_EXECUTE_FAILED   = -1006,
    CRAB_ERROR_FILTER_FAILED    = -1007,
    CRAB_ERROR_NO_MEMORY        = -1008
} CrabRuntimeError;
```

## 6. registry/tool_registry.h —— 注册表（数据结构）

```c
#define CRAB_REGISTRY_MAX_TOOLS 32

typedef struct {
    CrabToolSpec tools[CRAB_REGISTRY_MAX_TOOLS];
    size_t toolCount;
} CrabToolRegistry;
```

## 7. 类型一致性说明

- `CrabToolCall` 是 kernel 适配器解析 LLM 调用的产物，也是 `execute/filter` 回调的入参。
- `CrabToolResponse` 是 `CrabRuntimeCallTool` 的最终产物，其 `content` 是返回给 LLM 的文本（含 `tool: / success: / exit_code: / summary: / content: / truncated: / raw_ref:` 的标记段落）。
- 契约层保持**无逻辑、纯类型**，方便独立测试。

## 8. 复现要点（检查清单）

- [ ] 数据结构字段与容量常量一致。
- [ ] `CrabToolValue.stringValue` 内存所有权约定明确（不拥有 / FreeCall 释放）。
- [ ] execute/filter 回调签名与契约一致。
- [ ] 错误码覆盖注册、校验、执行、过滤、内存各失败场景。
