# 第 1 步实施设计：让 Tool 启用状态真正生效

状态：设计完成，待评审确认后实施  
对应计划：[安全能力渐进式实施计划](security_incremental_implementation_plan.md#42-第-1-步让-tool-启用状态真正生效)  
设计基线：2026-09-02

## 1. 本步骤目标

让 `CrabToolSpec.enabledByDefault` 从描述性元数据变成不可绕过的基础执行约束，形成最小的“双重检查”闭环：

1. Tool 暴露前过滤：默认禁用的 Tool 不进入发送给 LLM 的 Tool schema。
2. Tool 执行前检查：即使 LLM、测试程序或其他调用方直接构造 Tool call，Runtime 仍拒绝执行默认禁用的 Tool。

本步骤完成后，当前内置 `shell` 将满足：

- 仍注册在 Runtime 完整 Tool 注册表中；
- 不出现在发给 LLM 的 Tool schema 中；
- 伪造或幻觉产生的 `shell` Tool call 可以被识别；
- Runtime 返回稳定的 `CRAB_ERROR_TOOL_DISABLED`；
- `do_shell` 不会被调用，不产生文件、进程等执行副作用。

## 2. 范围与非目标

### 2.1 本步骤包含

- 严格定义 `enabledByDefault` 的有效值。
- 过滤 LLM 可见 Tool。
- 在 Runtime 唯一现有执行入口增加启用检查。
- 定义稳定错误码和错误优先级。
- 更新受影响的单元测试、安全测试和 Agent 矩阵测试。

### 2.2 本步骤不包含

- 不增加配置开关来重新启用 `shell`。
- 不实现 Principal、Capability 或完整 Policy Engine。
- 不实现用户审批。
- 不修改 `shell` 的执行器、timeout、rlimit 或网络隔离。
- 不改变 `read`、`write`、`edit`、`grep`、`glob`、`csv_read`、`skill_read`、`skill_complete` 的默认启用状态。
- 不根据 Session、用户或 Skill 动态生成 Tool 集合。
- 不把 `permissions`、`riskLevel`、`concurrency` 接入决策。

因此，在后续安全 Profile 和 Policy Engine 完成前，`enabledByDefault == 0` 表示该 Tool 在当前发布路径中实际禁用，而不仅仅是“默认关闭但可由配置开启”。显式启用机制属于后续计划，不在本步骤提前设计临时后门。

## 3. 当前代码状态

### 3.1 Tool 注册

`src/runtime/builtin_tools.c` 中的内置 Tool 通过 `CrabRuntimeRegisterBuiltinTools` 全部注册到 `CrabToolRegistry`。当前共有 9 个 Tool：

| Tool | `enabledByDefault` | 本步骤后的默认结果 |
|---|---:|---|
| `read` | 1 | 暴露并允许进入现有执行链 |
| `csv_read` | 1 | 暴露并允许进入现有执行链 |
| `write` | 1 | 暴露并允许进入现有执行链 |
| `edit` | 1 | 暴露并允许进入现有执行链 |
| `grep` | 1 | 暴露并允许进入现有执行链 |
| `glob` | 1 | 暴露并允许进入现有执行链 |
| `shell` | 0 | 不暴露，执行时拒绝 |
| `skill_read` | 1 | 暴露并允许进入现有执行链 |
| `skill_complete` | 1 | 暴露并允许进入现有执行链 |

注册阶段当前只校验名称、描述、执行函数、过滤函数、容量和重名，没有校验 `enabledByDefault` 是否为严格布尔值。

### 3.2 Tool schema 渲染

`src/kernel/kernel.c` 中的 `LlmToolAdapterRenderOpenaiToolsJson` 当前遍历完整 Catalog，并无条件渲染每个 Tool。因此 `shell` 会进入 `toolsJson`。

`AgentLoopInit` 在启动时生成一次全局 `toolsJson`。本步骤的启用状态仍是静态元数据，因此启动时生成一次是合理的，不需要改成每轮请求重新生成。

### 3.3 Tool 解析与执行

LLM 返回 Tool call 后，当前调用链为：

```text
LLM response
    |
    v
LlmToolAdapterParseCall(full catalog)
    |
    v
CrabRuntimeCallTool
    |
    +--> WorkingMemoryBeforeTool
    +--> CrabRegistryFind
    +--> readonlyMode check
    +--> CrabToolValidateCall
    +--> spec->execute
    +--> spec->filter
    +--> WorkingMemoryAfterTool
```

`CrabRuntimeCallTool` 当前没有检查 `enabledByDefault`。此外，`WorkingMemoryBeforeTool` 在 Tool 查找和校验之前执行，导致被拒绝的未知或非法调用也可能进入 Working Memory 生命周期。

### 3.4 已知测试影响

- `tests/test_main.c` 当前断言 `shell` 能默认执行成功。
- `tests/test_agent_matrix.py` 当前断言请求包含 9 个 Tool。
- `tests/test_agent_matrix.py` 的 `multi-tool` 场景主动调用 `shell` 并期待 `MATRIX_SHELL_OK`。
- 第 0 步新增的 `tests/test_security.c` 尚未覆盖默认禁用 Tool。

这些测试必须随本步骤调整，否则正确的安全行为会被旧测试当成回归。

## 4. 设计决策

### 4.1 区分完整注册表与 LLM 暴露集合

本步骤不从 Registry 删除 `shell`，也不让 `CrabRuntimeGetToolCatalog` 返回一个物理裁剪后的数组。

保留两个逻辑集合：

```text
完整注册表 Registered Tools
  用途：名称查找、参数解析、执行前拒绝、未来审计

默认暴露集合 Exposed Tools
  条件：enabledByDefault == 1
  用途：生成发送给 LLM 的 Tool schema
```

原因：如果 `shell` 完全从注册表删除，恶意或异常 LLM 返回 `shell` 时只能得到 `tool not found` 或解析错误，无法稳定地区分“程序根本没有这个 Tool”和“程序知道这个 Tool，但当前明确禁用”。保留完整注册表可以在 Runtime 返回更准确的 `CRAB_ERROR_TOOL_DISABLED`，也为后续安全审计提供稳定事件类型。

### 4.2 Runtime 是最终强制边界

Tool schema 过滤只能降低 LLM 主动选择禁用 Tool 的概率，不构成授权。Runtime 必须再次检查启用状态，因为 Tool call 还可能来自：

- LLM 幻觉或恶意 Provider；
- Session 中恢复的历史调用；
- 未来插件或其他 Kernel 调用路径；
- 直接调用 Runtime API 的本地代码；
- 测试或错误集成代码。

### 4.3 严格布尔语义

仅 `enabledByDefault == 1` 表示启用；`0` 表示禁用。其他值不按 C 的“非零即真”处理，而是在注册阶段返回 `CRAB_ERROR_INVALID_TOOL_SPEC`。

这可以防止未初始化值、错误 manifest 转换或未来插件注册代码意外开启 Tool。

### 4.4 禁用检查先于参数校验

找到 Tool 后，应先检查启用状态，再解析执行语义中的参数合法性。这样调用者无法利用禁用 Tool 获取更细的参数 schema 错误，也确保所有禁用调用得到统一结果。

建议错误优先级：

```text
1. Runtime / call / response 指针无效 -> CRAB_ERROR_INVALID_ARG
2. Tool 名称未注册              -> CRAB_ERROR_TOOL_NOT_FOUND
3. Tool 默认禁用                -> CRAB_ERROR_TOOL_DISABLED
4. Runtime 只读且 Tool 可写      -> CRAB_ERROR_PERMISSION
5. Tool 参数非法                 -> CRAB_ERROR_INVALID_ARG
6. execute / filter 结果          -> 保持现有行为
```

### 4.5 被拒绝的调用不进入 Working Memory Tool 生命周期

`WorkingMemoryBeforeTool` 应移动到以下基础检查全部通过之后：

- Tool 已注册；
- Tool 已启用；
- readonly 检查通过；
- Tool 参数校验通过。

这样禁用或非法 Tool 不会污染 Tool history。该调整不会改变合法且已启用 Tool 的执行顺序。

## 5. API 与数据结构修改

### 5.1 新增错误码

文件：`include/litecrab/runtime.h`

在 `CrabRuntimeError` 末尾追加错误码，保留现有数值不变：

```c
CRAB_ERROR_PERMISSION = -1009,
CRAB_ERROR_TOOL_DISABLED = -1010
```

必须追加而不是插入，以避免改变现有错误码数值。

禁用 Tool 的 `CrabToolResponse` 约定为：

```text
success      = 0
runtimeError = CRAB_ERROR_TOOL_DISABLED
toolExitCode = 0
summary      = "tool is disabled"
content      = 由 CrabBuildRuntimeError 生成的现有结构化文本
```

`toolExitCode` 保持 0，因为 Tool execute 根本没有运行；这是 Runtime 前置拒绝，不是 Tool 执行失败。

### 5.2 共享启用判断

文件：

- `include/litecrab/runtime.h`
- `src/runtime/runtime.c`

新增一个只表达基础元数据状态的判断函数：

```c
int CrabToolSpecIsDefaultEnabled(const CrabToolSpec* spec);
```

语义：仅当 `spec != NULL && spec->enabledByDefault == 1` 时返回真。

Renderer 和 Runtime 都调用该函数，避免两个位置分别使用不同的“真值”规则。后续 Policy Engine 会在这一基础 gate 之上叠加上下文策略，而不是删除该基础约束。

## 6. 文件级修改说明

### 6.1 `include/litecrab/runtime.h`

修改内容：

1. 在 `CrabRuntimeError` 末尾新增 `CRAB_ERROR_TOOL_DISABLED = -1010`。
2. 声明 `CrabToolSpecIsDefaultEnabled`。

不修改：

- `CrabToolSpec` 字段布局；
- `CrabRuntimeCallTool` 函数签名；
- 现有错误码数值；
- Tool catalog 结构。

### 6.2 `src/runtime/runtime.c`

修改内容一：实现共享判断函数。

```c
int CrabToolSpecIsDefaultEnabled(const CrabToolSpec* spec) {
    return spec && spec->enabledByDefault == 1;
}
```

修改内容二：在 `CrabRegistryRegister` 中严格校验 `enabledByDefault`：

```text
值为 0 或 1 -> 继续注册
其他值      -> CRAB_ERROR_INVALID_TOOL_SPEC
```

修改内容三：重排 `CrabRuntimeCallTool` 的前置检查：

```text
校验入口参数
  -> 查找 Tool
  -> enabledByDefault 检查
  -> readonly 检查
  -> 参数校验
  -> WorkingMemoryBeforeTool
  -> execute / filter / response
  -> WorkingMemoryAfterTool
```

禁用分支使用：

```c
return CrabBuildRuntimeError(
    response, call, CRAB_ERROR_TOOL_DISABLED, "tool is disabled");
```

必须保证禁用分支发生在 `spec->execute`、`WorkingMemoryBeforeTool` 和所有文件/进程副作用之前。

### 6.3 `src/kernel/kernel.c`

修改函数：`LlmToolAdapterRenderOpenaiToolsJson`

在遍历 Catalog 时跳过默认禁用 Tool。逗号逻辑不能继续依赖原始数组下标 `i`，必须依据实际已经输出的 Tool 数量：

```c
size_t emitted = 0;
for (...) {
    const CrabToolSpec* tool = ...;
    if (!CrabToolSpecIsDefaultEnabled(tool))
        continue;
    if (emitted++)
        append_comma();
    render_tool(tool);
}
```

这样可以正确处理：

- 第一个 Tool 被禁用；
- 中间 Tool 被禁用；
- 最后一个 Tool 被禁用；
- 所有 Tool 都被禁用，此时输出合法 JSON `[]`。

以下流程保持不变：

- `AgentLoopInit` 仍获取完整 Catalog；
- `toolsJson` 仍在启动时生成一次；
- `LlmToolAdapterParseCall` 仍使用完整 Catalog；
- `SkillRouterInit` 仍接收完整 Catalog，目前该函数不消费 Tool 内容。

### 6.4 `src/runtime/builtin_tools.c`

本步骤实施时不需要修改该文件的逻辑。

需要人工复核但保持不变：

```c
SPEC("shell", ..., 0, ..., CRAB_TOOL_RISK_HIGH, ..., do_shell)
```

`shell` 已正确声明 `enabledByDefault = 0`。本步骤的目标是让现有声明生效，而不是再次修改声明或改造 `do_shell`。

### 6.5 `tests/test_security.c`

新增一个独立测试组，例如：

```text
disabled tool is hidden and cannot execute
```

测试步骤：

1. 创建临时 workspace。
2. 初始化 Runtime 并注册内置 Tool。
3. 获取完整 Catalog，断言 Registry 仍有 9 个 Tool。
4. 渲染 Tool schema，断言 JSON 合法且不包含 `shell`。
5. 通过 `LlmToolAdapterParseCall` 使用完整 Catalog 解析伪造的 `shell` 调用。
6. shell 脚本内容尝试在 workspace 创建 marker 文件。
7. 调用 `CrabRuntimeCallTool`。
8. 断言返回 `CRAB_ERROR_TOOL_DISABLED`。
9. 断言 `response.runtimeError == CRAB_ERROR_TOOL_DISABLED`。
10. 断言 `response.toolExitCode == 0` 且 `response.success == 0`。
11. 断言 marker 文件不存在。

另增加自定义 Tool spec 测试，验证 `enabledByDefault = 2` 时注册返回 `CRAB_ERROR_INVALID_TOOL_SPEC`。

### 6.6 `tests/test_main.c`

修改现有测试，不删除对完整注册表的检查：

- 保留 `rt.registry.toolCount == 9`，证明 `shell` 仍在完整注册表中。
- 在 schema 测试中增加 `shell` 不存在的断言。
- 将当前期待 `shell-ok` 的测试改为期待 `CRAB_ERROR_TOOL_DISABLED`。
- 使用创建 marker 文件的脚本并断言文件不存在，证明 `do_shell` 没有运行。
- 保留同一测试函数中的 `skill_read` 正常和路径拒绝测试。

### 6.7 `tests/test_agent_matrix.py`

该文件不在初版简要清单中，但根据当前代码检查属于必改文件。

修改内容一：请求结构检查。

- Tool schema 数量从 9 变为 8。
- 除检查数量外，还应检查 `shell` 不在 Tool 名称集合中。
- 检查预期的 8 个默认启用 Tool 均存在，避免仅靠数量产生误判。

修改内容二：保留 `multi-tool` 测试目的。

当前 `multi-tool` 使用 `grep + glob + shell` 测试一次响应中的多 Tool 调用。将 `shell` 替换为另一个默认启用且无副作用的只读 Tool，例如：

```text
grep + glob + read(README.md)
```

并将成功 marker 从 `MATRIX_SHELL_OK` 改为 `README.md` 中的稳定内容。该测试继续验证多 Tool 解析、顺序和结果聚合，不再依赖默认禁用能力。

修改内容三：增加恶意/异常 Provider 场景。

增加 `disabled-tool` prompt。Mock Provider 即使没有在请求 schema 中看到 `shell`，仍主动返回一个 `shell` Tool call。下一轮检查 Tool result 是否包含：

- `runtime_error: -1010`；
- `tool is disabled`；
- marker 文件未生成。

该场景验证真实 Agent 数据流中“未暴露不等于不会收到调用”，并证明 Runtime 二次检查有效。

## 7. 修改文件汇总

| 文件 | 类型 | 修改目的 |
|---|---|---|
| `include/litecrab/runtime.h` | 生产代码 | 新增禁用错误码和共享启用判断声明 |
| `src/runtime/runtime.c` | 生产代码 | 严格注册校验、执行前禁用检查、拒绝调用不进入 Working Memory |
| `src/kernel/kernel.c` | 生产代码 | 仅渲染默认启用 Tool，修正过滤后的 JSON 逗号逻辑 |
| `tests/test_security.c` | 安全测试 | 验证隐藏、直接调用拒绝、稳定错误码和无副作用 |
| `tests/test_main.c` | 单元回归 | 更新旧 shell 成功预期并保留完整注册表测试 |
| `tests/test_agent_matrix.py` | Agent E2E | 更新 Tool schema、替换正常多 Tool 用例、增加伪造 shell 用例 |
| `docs/security/security_incremental_implementation_plan.md` | 计划文档 | 实施完成后记录状态、验证结果并链接本文档 |

本步骤预计不修改：

- `src/runtime/builtin_tools.c`：`shell` 的默认禁用声明已经正确。
- `CMakeLists.txt`：第 0 步已经注册安全测试目标。
- `tests/test_security_e2e.py`：本步骤的 LLM Tool 数据流由 `test_agent_matrix.py` 覆盖。

## 8. 修改后的数据流总结

### 8.1 服务启动与 Tool 暴露

```text
CrabRuntimeRegisterBuiltinTools
    |
    | 注册 9 个 Tool；enabledByDefault 必须严格为 0/1
    v
CrabToolRegistry（完整注册表，包含 shell）
    |
    v
CrabRuntimeGetToolCatalog
    |
    v
LlmToolAdapterRenderOpenaiToolsJson
    |
    | 仅保留 enabledByDefault == 1
    v
toolsJson（8 个 Tool，不包含 shell）
    |
    v
LlmChatToolsEx -> LLM Provider
```

### 8.2 正常启用 Tool 调用

```text
LLM 返回 read
    |
LlmToolAdapterParseCall（完整 Catalog 中找到 read）
    |
CrabRuntimeCallTool
    |
    +-- 已注册：是
    +-- 默认启用：是
    +-- readonly：通过
    +-- 参数校验：通过
    |
WorkingMemoryBeforeTool
    |
read.execute -> filter -> response
    |
WorkingMemoryAfterTool
    |
结果返回 LLM
```

### 8.3 伪造的禁用 Tool 调用

```text
恶意/异常 LLM 返回 shell
    |
LlmToolAdapterParseCall（完整 Catalog 中识别 shell）
    |
CrabRuntimeCallTool
    |
    +-- 已注册：是
    +-- 默认启用：否
    |
    +--> CRAB_ERROR_TOOL_DISABLED
             |
             +-- 不调用 WorkingMemoryBeforeTool
             +-- 不调用 do_shell
             +-- 不启动子进程
             +-- 不创建 marker 文件
             +-- 返回稳定 Runtime error response
```

### 8.4 未注册 Tool 调用

```text
LLM 返回 unknown_tool
    |
LlmToolAdapterParseCall
    |
    +--> tool not found / parse error
    |
不进入 Runtime execute
```

## 9. 测试矩阵

| 场景 | 预期结果 | 副作用断言 |
|---|---|---|
| 渲染当前内置 Catalog | 合法 JSON，8 个 Tool，无 `shell` | 无 |
| Catalog 第一个 Tool 禁用 | JSON 不以逗号开头 | 无 |
| Catalog 中间 Tool 禁用 | 前后对象逗号正确 | 无 |
| Catalog 全部 Tool 禁用 | 输出 `[]` | 无 |
| `enabledByDefault = 2` | 注册失败，`CRAB_ERROR_INVALID_TOOL_SPEC` | Registry 不增加 Tool |
| 直接调用 `shell` | `CRAB_ERROR_TOOL_DISABLED` | marker 文件不存在、无子进程结果 |
| 伪造 LLM `shell` call | 返回禁用 Tool result | marker 文件不存在 |
| 正常调用 `read` | 行为与当前一致 | 正常读取 |
| readonly Runtime 调用 `write` | 仍为 `CRAB_ERROR_PERMISSION` | 文件不存在 |
| 调用未知 Tool | 仍为 `CRAB_ERROR_TOOL_NOT_FOUND` 或 adapter parse error | 无执行副作用 |
| Agent request shape | 8 个默认启用 Tool | `shell` 不在名称集合 |
| 多 Tool 正常调用 | 只读 Tool 均成功 | 不依赖 shell |

## 10. 兼容性影响

### 10.1 明确的行为变化

- LLM 请求中的 Tool 数量由 9 变为 8。
- LLM 不再看到 `shell` 描述和参数 schema。
- 直接调用 `CrabRuntimeCallTool(..., "shell", ...)` 从成功执行变为 `CRAB_ERROR_TOOL_DISABLED`。
- 依赖通用 `shell` 的 Skill 在后续受限执行器完成前不能执行 shell 步骤。

这些变化是本步骤的安全目标，不属于回归。

### 10.2 保持兼容的部分

- 完整 Registry 仍包含 9 个 Tool。
- `CrabRuntimeGetToolCatalog` 的签名和返回布局不变。
- `CrabRuntimeCallTool` 的签名不变。
- 已启用 Tool 的 schema、参数校验、execute 和 filter 行为不变。
- Session、Gateway、LLM Provider 和配置格式不变。

### 10.3 已知暂时限制

- 本步骤后没有安全的 `shell` 开启方式。
- `enabledByDefault` 仍是全局静态状态，不支持按用户或 Session 区分。
- Tool 是否启用尚未产生专门安全审计事件。
- 对 write/edit 的进一步限制需要后续 Policy、审批和安全路径步骤。

## 11. 实施顺序

建议按以下顺序修改，每一步都可快速编译定位问题：

1. 在 `runtime.h` 追加错误码并声明共享判断函数。
2. 在 `runtime.c` 实现严格启用判断和注册校验。
3. 在 `tests/test_security.c` 先加入注册值校验测试。
4. 修改 Renderer 跳过禁用 Tool，加入空集合和逗号测试。
5. 修改 Runtime 调用顺序和禁用拒绝，加入无副作用测试。
6. 更新 `tests/test_main.c` 的旧 shell 预期。
7. 更新 `tests/test_agent_matrix.py` 的请求结构和多 Tool 测试。
8. 增加 Agent 矩阵中的伪造 `shell` 调用测试。
9. 运行独立安全测试、相关 Agent E2E 和全量回归。
10. 验收通过后更新总计划中的实施状态和记录。

## 12. 验证命令与通过标准

应使用全新或重新配置的构建目录：

```sh
cmake -S . -B build-step1 -DLITECRAB_ENABLE_STATIC_LINK=OFF
cmake --build build-step1 -j2
ctest --test-dir build-step1 -R litecrab_security --output-on-failure
ctest --test-dir build-step1 -R litecrab_agent_matrix --output-on-failure
ctest --test-dir build-step1 --output-on-failure
```

通过标准：

1. `litecrab_security_tests` 全部通过。
2. `litecrab_agent_matrix` 的 stream 和 non-stream 两组均通过。
3. 全量 8 项测试全部通过。
4. Tool schema 是合法 JSON，只包含 8 个默认启用 Tool。
5. 直接和经 LLM 数据流构造的 `shell` 调用都返回 `CRAB_ERROR_TOOL_DISABLED`。
6. 禁用 shell 测试的 marker 文件不存在。
7. `do_shell` 没有执行，Tool response 中 `toolExitCode == 0`。
8. 合法已启用 Tool 的既有行为没有变化。
9. `git diff --check` 无错误。
10. 生产代码变更只落在本步骤列出的 Runtime/Kernel 文件中。

## 13. 回退边界

本步骤不迁移持久化数据、不修改配置格式，也不改变公开结构体布局，因此可以整体按单一提交回退。

回退时必须同时回退：

- Renderer 的 Tool 过滤；
- Runtime 的禁用检查；
- 新错误码；
- 对应测试预期。

不能只回退 Runtime 检查而保留 schema 过滤，也不能只回退 schema 过滤而保留测试；否则暴露行为、执行行为和测试契约会重新不一致。

## 14. 评审重点

实施前建议重点确认以下结论：

1. 当前阶段 `shell` 是否接受“完全不可用”，直到后续受限执行器或安全 Profile 提供显式开启方式。
2. 是否接受完整 Registry 保留 `shell`，但 LLM 暴露集合排除 `shell`。
3. 是否接受禁用 Tool 优先于参数校验返回，统一使用 `CRAB_ERROR_TOOL_DISABLED`。
4. 是否接受将 `WorkingMemoryBeforeTool` 移到所有基础检查通过之后。
5. 是否接受 Agent 矩阵将正常多 Tool 测试中的 `shell` 替换为只读 Tool，并另设恶意 Provider 用例验证禁用行为。

上述五点确认后，本步骤不存在需要扩展到 Policy、审批或沙箱模块的前置依赖。

