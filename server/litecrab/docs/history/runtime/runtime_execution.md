# runtime_execution：参数校验与结果构建

## 1. 模块职责

本文档覆盖 runtime 中两个“执行支撑”模块：

- **args/tool_args**：工具调用参数的查找、校验与类型安全取值。
- **result/result_builder**：原始结果 / 过滤结果的操作、标准过滤器，以及最终响应的组装。

二者配合 `runtime_call`（见 runtime_overview.md）完成一次完整工具调用。

## 一、tool_args（参数校验）

### 1. 接口

```c
const CrabToolArg *CrabToolCallFindArg(const CrabToolCall *call, const char *name);
const CrabToolParamSpec *CrabToolSpecFindParam(const CrabToolSpec *tool, const char *name);
int  CrabToolValidateCall(const CrabToolSpec *tool, const CrabToolCall *call,
                          char *error, size_t errorSize);
const char *CrabToolArgsGetString(const CrabToolCall *call, const char *name, const char *defaultValue);
int64_t CrabToolArgsGetInt(const CrabToolCall *call, const char *name, int64_t defaultValue);
int  CrabToolArgsGetBool(const CrabToolCall *call, const char *name, int defaultValue);
```

### 2. 查找

- `CrabToolCallFindArg`：在 `call->args[0..argCount)` 中按名称线性查找。
- `CrabToolSpecFindParam`：在 `tool->params[0..paramCount)` 中按名称线性查找。

### 3. 校验（CrabToolValidateCall）—— 核心

```
CrabToolValidateCall(tool, call, error, errorSize):
    # 基础
    if !tool || !call: "invalid call"; return INVALID_ARG
    if strcmp(call.toolName, tool.name)!=0: "tool name mismatch"; return INVALID_ARG
    if call.argCount > CRAB_TOOL_MAX_ARGS: "too many arguments"; return INVALID_ARG

    # 逐参数校验
    for i in 0..argCount-1:
        arg = call.args[i]
        if arg.name 为空: "empty argument name"; return INVALID_ARG
        if 名称重复 (只向前查): "duplicate argument"; return INVALID_ARG

        param = FindParam(tool, arg.name)
        if !param: "unknown argument"; return INVALID_ARG
        if arg.value.type != param.type: "invalid argument type"; return INVALID_ARG
        if type==STRING 且 stringValue 为 NULL: "missing string value"; return INVALID_ARG
        if type==INT 且 (minInt!=0 || maxInt!=0):
            if 越界: "integer out of range"; return INVALID_ARG
        if type==STRING:
            if !StringInEnum(arg.stringValue, param.enumValues, param.enumCount):
                "invalid enum value"; return INVALID_ARG

    # 必填参数检查
    for i in 0..paramCount-1:
        param = tool.params[i]
        if param.required && !FindArg(call, param.name):
            "missing required argument"; return INVALID_ARG

    error[0]='\0'
    return OK
```

要点：
- **重复检测**只向前比较（`beforeIndex`），O(n²) 但 n 上限 16，可接受。
- 枚举校验：`enumCount==0` 时视为不限枚举。
- 必填缺失在最后统一检查。

### 4. 类型安全取值

- `CrabToolArgsGetString`：若参数不存在或类型非 STRING 或值为 NULL，返回 defaultValue。
- `CrabToolArgsGetInt`：非 INT 返回 defaultValue。
- `CrabToolArgsGetBool`：非 BOOL 返回 defaultValue，否则返回 `boolValue?1:0`。

这些是内置工具默认值机制的基础（如 read 的 `maxLines` 默认 200）。

## 二、result_builder（结果构建）

### 1. 原始结果操作

```c
void CrabRawResultClear(CrabRawResult *raw);         /* 释放 stdout/stderr 并清零 */
int  CrabRawResultSetStdout(CrabRawResult *raw, const char *data);  /* 深拷贝 */
int  CrabRawResultSetStderr(CrabRawResult *raw, const char *data);
```

- `SetStdout/SetStderr` 会 `free` 旧指针、`strdup` 新内容、更新 size；`data==NULL` 视为空串。

### 2. 过滤结果操作

```c
int CrabFilteredResultSetSummary(CrabFilteredResult *filtered, const char *summary);
int CrabFilteredResultSetContent(CrabFilteredResult *filtered, const char *content);
int CrabFilteredResultAddWarning(CrabFilteredResult *filtered, const char *warning);
```

- 内部 `CrabCopyText` 用 `strncpy` 截断并强制 `\0` 结尾。
- `AddWarning` 在超过 `CRAB_TOOL_WARNING_MAX(8)` 时返回错误。

### 3. 标准过滤器（CrabBasicFilter）

```
CrabBasicFilter(call, raw, filtered):
    memset(filtered,0)
    filtered.success      = raw.exitCode == 0
    filtered.truncated    = raw.runtimeTruncated
    filtered.rawAvailable = raw.rawRef[0] != '\0'

    if raw.exitCode == 0:
        summary = "tool completed"
        content = raw.stdoutData ?: ""
    else:
        summary = raw.stderrData ?: "tool failed"
        content = raw.stdoutData ?: ""

    if exitCode==0 且 stderrData 非空: AddWarning(stderrData)
    return OK
```

这是大多数内置工具共用的 filter 实现。

### 4. 组装工具响应（CrabBuildToolResponse）

```
CrabBuildToolResponse(tool, call, raw, filtered, response):
    CrabResponseInit(response, call)     # 拷贝 callId/toolName，清零
    response.success       = filtered.success
    response.runtimeError  = 0
    response.toolExitCode  = raw.exitCode
    response.filterError   = 0
    response.truncated     = filtered.truncated
    response.warningCount  = filtered.warningCount
    response.summary       = filtered.summary
    response.rawRef        = raw.rawRef
    拷贝 warnings[]

    # 组装 content（给 LLM 的文本标记格式）
    content = "tool: <name>\n"
              "success: true|false\n"
              "exit_code: <raw.exitCode>\n\n"
              "summary:\n  <filtered.summary>\n\n"
              "content:\n<filtered.content>\n"
              + (warnings 非空 ? "\nwarnings:\n  <w...>\n" : "")
              + "\ntruncated: true|false\n"
              + "raw_ref: <raw.rawRef>\n"
    return OK
```

`content` 中的 `exit_code:` 行是 skill_handler 提取退出码的依据（见 kernel_skill_handler.md）。

### 5. 运行时错误响应（CrabBuildRuntimeError）

```
CrabBuildRuntimeError(response, call, runtimeError, message):
    CrabResponseInit(response, call)
    response.success      = 0
    response.runtimeError = runtimeError
    response.toolExitCode = 0
    response.filterError  = 0
    response.summary      = message ?: "runtime error"
    content = "tool: <name>\n"
              "success: false\n"
              "runtime_error: <code>\n\n"
              "summary:\n  <summary>\n"
    return runtimeError
```

### 6. 兜底响应（CrabBuildFallbackResponse）

```
CrabBuildFallbackResponse(tool, call, raw, response):
    filtered.success = 0
    filtered.truncated = 1
    summary = "filter failed; returning fallback summary"
    content  = raw.stderrData ?: ""
    CrabBuildToolResponse(tool, call, raw, &filtered, response)
    response.filterError = CRAB_ERROR_FILTER_FAILED
    return CRAB_ERROR_FILTER_FAILED
```

## 三、复现要点（检查清单）

### args
- [ ] 校验覆盖：toolName 不匹配、参数过多、空名、重复名、未知参数、类型不匹配、整型越界、枚举越界、必填缺失。
- [ ] 取系统一返回 defaultValue，不崩溃。

### result
- [ ] SetStdout/SetStderr 深拷贝与旧内存释放正确。
- [ ] BasicFilter 对成功/失败分支的 summary/content 选择正确。
- [ ] BuildToolResponse 的 content 含 `exit_code:` 标记。
- [ ] 错误/兜底响应各自正确设置 runtimeError / filterError。

## 四、相关文档

- `runtime_contracts.md`：数据结构。
- `runtime_overview.md`：调用主流程。
- `runtime_builtin_tools.md`：内置工具如何使用这些支撑。
