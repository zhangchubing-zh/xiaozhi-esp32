# runtime_overview：运行时总体结构与调用主流程

## 1. 模块职责

运行时（`src/runtime`）是**工具执行环境**。它负责：

1. 管理工具注册表与工具生命周期。
2. 提供统一的工具调用入口 `CrabRuntimeCallTool`：查找 -> 校验参数 -> execute -> filter -> 组装响应。
3. 注册一组**内置工具**（文件读写、搜索、shell、技能读取等），供 Agent 调用。
4. 通过 `runtime_api` 对 kernel 层暴露唯一接口面。

## 2. 目录结构

```
src/runtime/
├── api/          runtime_api.h     # 对 kernel 的唯一入口
├── contract/     tool_call / tool_result / tool_schema / runtime_error
├── core/         runtime_context(生命周期+配置) / runtime_call(调用流程) / builtin_tools
├── registry/     tool_registry     # 工具注册表实现
├── args/         tool_args         # 参数查找/校验/取值
├── result/       result_builder    # 结果组装与过滤器
└── tools/        file / search / shell / skill   # 内置工具实现
```

## 3. 核心对象：CrabRuntime 与配置

```c
typedef struct {
    const char *workspaceRoot;    /* 工作空间根目录 */
    const char *rawResultDir;     /* 原始结果存储目录 */
    int readonlyMode;
    int maxResponseBytes;         /* 响应上限，默认 64KB */
    int maxRawBytes;              /* 原始结果上限，默认 1MB */
    int maxWarnings;              /* 警告上限，默认 8 */
    CrabRuntimeEmitFn emitMessage;/* 消息回调 */
    void *emitUserData;
    CrabRuntimeNowFn now;         /* 时间回调 */
    void *nowUserData;
} CrabRuntimeConfig;

struct CrabRuntime {
    CrabRuntimeConfig config;
    char workspaceRoot[CRAB_RUNTIME_PATH_MAX];  /* 1024 */
    char rawResultDir[CRAB_RUNTIME_PATH_MAX];
    CrabToolRegistry registry;    /* 工具注册表 */
    int started;
};
```

`config.workspaceRoot` / `rawResultDir` 在 Init 时被拷贝进定长数组，指针重指向内部数组，保证字符串生命周期。

## 4. 生命周期（runtime_context）

```
CrabRuntimeInit(runtime, config):
    memset(runtime,0)
    CrabRegistryInit(&runtime->registry)
    拷贝 config（若 config 为 NULL 则用默认 "."）
    归一化 maxResponseBytes/maxRawBytes/maxWarnings 默认值
    return OK

CrabRuntimeStart:  started=1
CrabRuntimeStop:   started=0
CrabRuntimeDestroy: memset(runtime,0)
CrabRuntimeGetWorkspaceRoot: 返回 workspaceRoot（空则 "."）
```

## 5. 对外 API（runtime_api.h）

```c
int  CrabRuntimeRegisterBuiltinTools(CrabRuntime *runtime);
int  CrabRuntimeGetToolCatalog(const CrabRuntime *runtime, CrabToolCatalogView *catalog);
int  CrabRuntimeCallTool(CrabRuntime *runtime, const CrabToolCall *call, CrabToolResponse *response);
```

- `RegisterBuiltinTools`：把所有内置工具的规格注册进 `runtime->registry`。
- `GetToolCatalog`：导出只读目录视图，供 LLM tools JSON 渲染。
- `CallTool`：核心执行入口（见第 6 节）。

## 6. 工具调用主流程（runtime_call）—— 核心算法

```
CrabRuntimeCallTool(runtime, call, response):
    if !runtime || !call || !response: return INVALID_ARG

    memset(&raw, 0); memset(&filtered, 0)

    # 1) 按名称查找工具
    tool = CrabRegistryFind(&runtime->registry, call->toolName)
    if !tool:
        return CrabBuildRuntimeError(response, call, TOOL_NOT_FOUND, "tool not found")

    # 2) 校验参数（对 call 参数做类型/必填/枚举/范围/重复校验）
    rc = CrabToolValidateCall(tool, call, argError, sizeof(argError))
    if rc != OK:
        return CrabBuildRuntimeError(response, call, rc, argError)

    # 3) 执行（原始结果写入 raw）
    executeRc = tool->execute(runtime, call, &raw)
    if executeRc != OK && raw.exitCode == 0:
        raw.exitCode = executeRc          # 统一退出码

    # 4) 过滤（生成有界内容 filtered）
    filterRc = tool->filter(call, &raw, &filtered)

    # 5) 组装响应
    if filterRc != OK:
        CrabBuildFallbackResponse(tool, call, &raw, response)   # 兜底
    else:
        CrabBuildToolResponse(tool, call, &raw, &filtered, response)

    CrabRawResultClear(&raw)              # 释放堆内存
    return executeRc
```

### 6.1 关键点

- **校验失败 / 找不到工具**：直接 `CrabBuildRuntimeError` 生成错误响应并返回错误码。
- **execute 返回值与 exitCode 的关系**：若 execute 返回非 0 但 raw.exitCode 仍为 0，用返回值回填 exitCode。
- **filter 失败兜底**：用 `CrabBuildFallbackResponse` 生成 `success=false`、summary 为 "filter failed; returning fallback summary" 的响应，并置 `filterError`。
- **最终返回 executeRc**（而非 filterRc），调用方据此判断执行是否成功。

## 7. 调用流程时序图

```
kernel (ToolExecutor)
   │  CrabRuntimeCallTool(runtime, call, &response)
   ▼
runtime_call:  registry.find -> tool
   │
   ├── 未找到 ──► CrabBuildRuntimeError(TOOL_NOT_FOUND)
   │
   ▼
   CrabToolValidateCall(args 校验)
   │  失败 ──► CrabBuildRuntimeError(rc, argError)
   ▼
   tool->execute(runtime, call, &raw)     # 产生 CrabRawResult
   ▼
   tool->filter(call, &raw, &filtered)    # 产生 CrabFilteredResult
   │
   ├── filter 失败 ──► CrabBuildFallbackResponse
   └── filter 成功 ──► CrabBuildToolResponse  → response.content 含标记文本
   ▼
   CrabRawResultClear(&raw)
   return executeRc
```

## 8. Registry 实现（tool_registry.c）

```
CrabRegistryInit:           清零, toolCount=0
CrabRegistryRegister(registry, tool):
    # 依次校验：数量已满(REGISTRY_FULL)、名称为空/描述为空(INVALID_TOOL_SPEC)、
    #           execute/filter 回调为空(INVALID_TOOL_SPEC)、同名重复(DUPLICATE_TOOL)
    registry.tools[toolCount++] = *tool
    return OK
CrabRegistryFind(registry, name):  线性查找返回规格指针
CrabRegistryGetCatalog(registry, catalog): 导出只读视图 {tools, toolCount}
```

## 9. 复现要点（检查清单）

- [ ] 生命周期 Init/Start/Stop/Destroy 行为正确，默认值归一化。
- [ ] `CallTool` 全流程：find -> validate -> execute -> filter -> response 顺序正确。
- [ ] 找不到工具 / 校验失败返回对应运行时错误。
- [ ] filter 失败时走 FallbackResponse 且不崩溃。
- [ ] raw 堆内存被正确清理，不泄漏。
- [ ] 注册表去重/满员/非法规格校验完整。

## 10. 相关文档

- `runtime_contracts.md`：数据结构与回调契约。
- `runtime_execution.md`：参数校验、结果构建细节。
- `runtime_builtin_tools.md`：内置工具实现。
