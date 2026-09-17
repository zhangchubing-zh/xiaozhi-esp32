# LiteCrab 安全能力渐进式实施计划

状态：已按当前代码复核；部分完成，继续实施
计划基线：2026-09-02  
最近复核：2026-09-12
适用项目：LiteCrab 端侧 Agent  
相关文档：[Security 模块架构设计与风险边界](security_module_architecture.md)

状态标识：**已完成**表示当前代码和测试满足本步骤主要验收条件；**部分实现**表示已有可复用子能力，但仍有验收项未满足；**未实现**表示尚无计划要求的权威模块或执行闭环。局部辅助逻辑不能替代完整安全能力。

## 1. 文档目的

本文档把 LiteCrab 的安全能力建设拆分成可独立讨论、实现、验证和回退的小步骤，避免一次修改多个安全边界后难以判断单项功能是否合理、是否真正生效。

后续实施遵守以下约束：

1. 每次只实施一个计划项，不跨项顺带重构。
2. 每项实施前先确认数据结构、API、默认行为和兼容性影响。
3. 每项都必须具备独立测试和明确验收条件。
4. 当前项未通过验收前，不进入下一项。
5. 安全文档只描述已经由可信代码强制执行的能力，不把 Prompt、注释或未使用的元数据视为安全能力。
6. 每个计划项保留独立提交点和回退点。

## 2. 当前基线

### 2.1 验证结果

2026-09-02 使用全新的 WSL 临时构建目录完成配置、编译和测试，源码对应的 6 项测试全部通过：

- `litecrab_tests`
- `litecrab_tcp_e2e`
- `litecrab_transfer_station`
- `litecrab_agent_matrix`
- `litecrab_session_restart`
- `litecrab_ui_session_transport`

仓库现有 `build/` 可能对应旧源码。后续验证必须使用全新 CMake 构建目录，不能直接把已有构建目录中的测试清单和二进制作为当前基线。

2026-09-12 使用全新 WSL 临时目录重新配置和编译，并运行 `litecrab_security_tests`、`litecrab_security_e2e`、`litecrab_long_running_stress`、`litecrab_deployment_contract`，4 项全部通过。该结果只证明当前已覆盖能力没有回归，不代表本计划所有步骤已经完成。

### 2.2 当前已落地的安全基础

- `shell.enabledByDefault = 0` 已由 schema Renderer 和 Runtime 执行入口共同强制；默认不向 LLM 暴露，也不能通过伪造 Tool call 直接执行。
- TCP 默认监听 `127.0.0.1`；非 loopback 必须显式设置 `allow_unauthenticated_remote=true`。
- Gateway 已有固定 Worker、连接总量、请求大小和 socket timeout；Hub 已有有界 lane、Request Registry、deadline 和取消。
- Kernel 已有固定 Run 轮数、LLM 调用数和 Tool 调用数；LLM 与 Tool 输出已有硬上限。
- `skill_read` 已使用 `openat + O_NOFOLLOW` 逐级读取；普通文件 Tool 仍使用路径检查后再打开。
- `exec_program` 已具备 typed argv、最小环境、进程组、部分 rlimit、wall timeout 和输出上限。
- Session、日志、Trace 和 Alarm spool 已有容量或轮转边界。

### 2.3 当前仍需解决的主要缺口

- Trace 仍记录原始用户输入、Tool 输入/输出和最终回答；当前只有“是否记录完整 LLM messages”的布尔开关，没有完整的 `redacted/full/off` 模式和统一 secret 脱敏。
- Observability 创建目录时仍使用 `0775`，日志和 Trace 使用 `fopen`，没有独立强制 `0700/0600`。
- Run/LLM/Tool 预算主要是编译期常量，没有统一配置和稳定 `budget_exceeded` reason code。
- TCP 仍没有可信客户端认证；请求中的 `userId` 仍由客户端提供。
- 没有 `CrabPrincipal`、`CrabSecurityContext`、Policy Decision Point、安全 Profile 或安全审计模块。
- 普通文件 Tool 尚未使用 root fd、逐级 `openat/O_NOFOLLOW`、protected path 和 hardlink/特殊文件控制。
- `exec_program` 允许调用参数选择 workspace 内任意可执行文件，不是固定 program ID/allowlist；也没有接入 Policy。
- 没有与 Principal、Session、ToolCall、参数 hash 和 policy revision 绑定的持久审批。
- 没有跨告警、跨重试的设备副作用 Mutation Ledger。

## 3. 安全框架蓝图

### 3.1 核心原则

模型只能提出动作，可信代码负责判断动作是否允许并执行。用户输入、LLM 输出、Tool 参数、Workspace 文件、Skill 内容和 Tool 输出都不能自行授予权限。

```text
                       不可信区域
 UI / TCP / IPC / 用户输入 / Workspace 文件 / LLM / Skill
                             |
                             v
+---------------- Gateway / Auth Boundary ----------------+
| 请求限长、连接限额、可信身份映射、Session 所有权检查       |
+--------------------------+------------------------------+
                           |
                    CrabSecurityContext
                           |
                           v
+---------------------- Agent Kernel ----------------------+
| Run 预算 / Context 数据标记 / 当前 Session 与 Skill 状态   |
+--------------------------+------------------------------+
                           |
              只向 LLM 暴露允许提出的 Tool
                           |
                           v
                    LLM Tool Proposal
                     （不可信数据）
                           |
                           v
+-------------------- Policy Engine -----------------------+
| Principal ∩ Profile ∩ Tool ∩ Capability ∩ Resource       |
|             ALLOW / DENY / REQUIRE_APPROVAL              |
+-------------+----------------------+---------------------+
              |                      |
            DENY              Persistent Approval
                                     |
                              批准后重新评估
                                     |
                                     v
+-------------------- Guarded Executor --------------------+
| 安全路径访问 / 受限进程 / 网络出口 / Secret 最小注入       |
+--------------------------+------------------------------+
                           |
                  Output Guard / Redaction
                           |
              +------------+-------------+
              v                          v
       返回模型/用户                 Security Audit
```

### 3.2 目标模块边界

以下是目标结构，不在第一步一次性创建。只有某项计划开始实施时，才新增该项需要的最小模块。

```text
include/litecrab/security.h       公共安全类型和稳定 reason code

src/security/
  context.c                      Principal、Session、Run、ToolCall 上下文
  policy.c                       确定性策略决策
  path.c                         dirfd/openat/O_NOFOLLOW 安全路径访问
  approval.c                     审批、中断、过期、重放防护
  process.c                      timeout、rlimit、环境与 fd 清理
  guardrail.c                    输入、Tool 输出、最终输出检查
  audit.c                        脱敏安全事件
  secret.c                       Secret handle 和按调用注入
  network.c                      Tool 网络出口策略
```

### 3.3 强制检查点

安全执行链必须至少保留以下三个检查点：

1. **Tool 暴露前**：只向 LLM 渲染当前上下文允许提出的 Tool。
2. **Tool 执行前**：依据规范化后的真实参数和资源重新决策。
3. **审批完成后**：再次检查 Principal、Session、参数、策略版本、资源状态和审批有效期。

Tool schema 过滤只是缩小攻击面，不能代替执行前检查。审批也不能跳过重新决策。

## 4. 渐进式实施计划

### 4.0 当前状态总览

| 步骤 | 当前状态 | 已落地摘要 | 完成该步骤仍需补齐 |
|---|---|---|---|
| Step 0 安全回归基座 | 已完成 | 独立安全单测和 E2E | Audit 建立后补安全事件断言 |
| Step 1 Tool 启用状态 | 已完成 | schema 隐藏、Runtime 二次拒绝、稳定错误码 | 无本步骤阻塞项 |
| Step 2 Gateway 暴露 | 已完成 | 默认 loopback、双层绑定校验、远程风险开关 | 认证属于 Step 6，不计入本步骤 |
| Step 3 Trace 最小化 | 部分实现 | LLM messages 默认不落盘、截断、轮转 | 原文最小化、secret 脱敏、模式配置、0700/0600 |
| Step 4 Run/Tool 预算 | 部分实现 | 固定轮数、LLM/Tool 次数、deadline、响应上限 | 配置化、统一预算状态、稳定 reason code |
| Step 5 Gateway 预算 | 部分实现 | 固定 Worker、连接/请求/timeout 上限、busy 拒绝 | 绝对帧 deadline 或 Reactor，防持续慢速发送 |
| Step 6 安全上下文 | 未实现 | 无 | Principal、SecurityContext、可信身份贯通 |
| Step 7 Policy Engine | 未实现 | 仅有局部 Tool admission | 三态决策、统一入口、policy revision |
| Step 8 Security Profile | 未实现 | 无 | safe/standard/developer 和严格配置 |
| Step 9 安全文件访问 | 部分实现 | `skill_read` 已逐级 `openat/O_NOFOLLOW` | 普通文件 Tool、protected path、原子安全写入 |
| Step 10 审批存储 | 未实现 | 无 | 持久审批、过期、参数与身份绑定 |
| Step 11 审批接入 Agent | 未实现 | Skill 恢复引用可复用部分结构 | Policy interruption 和恢复前重新决策 |
| Step 12 受限进程 | 部分实现 | `exec_program` 的 argv、rlimit、进程组、timeout、输出限制 | 固定 program registry、Policy、进程数/更强隔离 |

状态判断依据是当前源码中的实际强制点和测试，而不是旧文档中的“计划”或 Prompt 中的行为要求。

## 4.1 第 0 步：建立安全回归基座

实施状态：**已完成（2026-09-02；2026-09-12 复核仍有效）**

### 实现功能

本步骤不改变产品运行行为，只建立独立的安全测试入口。后续每项安全测试必须同时验证：

- 被拒绝的动作确实没有产生副作用；
- 返回稳定、可机器判断的 reason code；
- 生成正确的脱敏审计事件；
- 原有非安全功能没有发生非预期回归。

### 修改文件

- 修改 `CMakeLists.txt`
- 新增 `tests/test_security.c`
- 新增 `tests/test_security_e2e.py`
- 视测试公共辅助函数需要，少量调整 `tests/test_main.c`

### 验收标准

- 全新配置和构建成功。
- 当前 6 项测试继续通过。
- 新安全测试可以独立运行。
- 安全测试失败时可以明确定位到一个安全控制。
- 不再把陈旧 `build/` 中的测试清单作为源码基线。

### 实施记录

- 新增独立 C 测试目标 `litecrab_security_tests`，覆盖路径逃逸、目录符号链接逃逸、只读拒绝、未知 Tool 参数和 Session ID 边界。
- C 测试同时断言稳定错误码和目标文件未生成，避免仅检查错误文本。
- 新增独立 Gateway 测试 `litecrab_security_e2e`，覆盖非法 Session ID 和超长请求拒绝，并验证拒绝后服务仍可接受新连接。
- 安全审计事件将在审计模块建立后加入相应断言；第 0 步不通过修改生产代码伪造尚不存在的审计能力。
- 使用全新 WSL 构建目录完成验证，8 项测试全部通过。

## 4.2 第 1 步：让 Tool 启用状态真正生效

实施状态：**已完成（2026-09-02；2026-09-12 复核仍有效）**

原详细设计已归并到 [Security 模块架构设计](security_module_architecture.md) 的 Tool 强制执行和能力状态章节。

### 实现功能

- `enabledByDefault == 0` 的 Tool 不再渲染给 LLM。
- 即使模型或调用方直接构造 Tool call，Runtime 执行入口仍会再次拒绝。
- 新增稳定错误码 `CRAB_ERROR_TOOL_DISABLED = -1010`。
- `shell` 默认不可见、不可执行。
- 暂不引入完整 Policy Engine，不改变普通文件 Tool 的现有行为。

### 修改文件

- `include/litecrab/runtime.h`
- `src/runtime/runtime.c`
- `src/kernel/kernel.c`
- `tests/test_security.c`
- `tests/test_main.c`
- `tests/test_agent_matrix.py`

### 实施记录

- 在 `CrabRuntimeError` 末尾追加 `CRAB_ERROR_TOOL_DISABLED = -1010`，未改变现有错误码数值。
- 新增共享判断函数 `CrabToolSpecIsDefaultEnabled`，仅当 `spec != NULL && spec->enabledByDefault == 1` 时返回真；Renderer 与 Runtime 共用同一真值规则。
- `CrabRegistryRegister` 严格校验 `enabledByDefault` 必须为 `0` 或 `1`，其他值返回 `CRAB_ERROR_INVALID_TOOL_SPEC`。
- `LlmToolAdapterRenderOpenaiToolsJson` 跳过默认禁用 Tool，逗号逻辑改为按实际已输出 Tool 数量 `emitted` 计数，覆盖首/中/尾/全部禁用四种情况。
- `CrabRuntimeCallTool` 重排前置检查顺序：参数校验 -> Tool 查找 -> 启用检查 -> readonly 检查 -> 参数校验 -> `WorkingMemoryBeforeTool` -> execute/filter -> `WorkingMemoryAfterTool`。禁用分支稳定返回 `CRAB_ERROR_TOOL_DISABLED` 且 `toolExitCode == 0`，不调用 `do_shell`，不进入 Working Memory 生命周期。
- `tests/test_security.c` 新增三组测试：默认禁用 Tool 隐藏与直接调用拒绝、伪造 LLM `shell` call 经完整 Catalog 解析后被 Runtime 拒绝、`enabledByDefault = 2` 注册返回 `CRAB_ERROR_INVALID_TOOL_SPEC`。
- `tests/test_main.c` 保留 `rt.registry.toolCount == 9`，新增 schema 不包含 `shell` 断言，原期待 `shell-ok` 的测试改为期待 `CRAB_ERROR_TOOL_DISABLED` 并断言 marker 文件不存在。
- `tests/test_agent_matrix.py` 请求结构从 9 Tool 改为 8 Tool 并断言 `shell` 不在 Tool 名称集合中；`multi-tool` 用例将 `shell` 替换为只读 `read(README.md)`；新增 `disabled-tool` 场景验证伪造 `shell` call 返回 `runtime_error: -1010` 与 `tool is disabled`。
- 完整 Catalog 仍保留 9 个 Tool；`CrabRuntimeGetToolCatalog` 与 `CrabRuntimeCallTool` 签名不变。

### 验收结果

使用全新 WSL 构建目录 `build-step1` 完成配置、编译和测试：

- `litecrab_security_tests`：53 项 check 全部通过，覆盖隐藏、直接调用拒绝、伪造 LLM call 拒绝、严格布尔校验和无副作用。
- `litecrab_agent_matrix`：stream 和 non-stream 两组均通过，包含伪造 `shell` Tool call 场景。
- 全量 8 项测试全部通过（`litecrab_tests`、`litecrab_tcp_e2e`、`litecrab_transfer_station`、`litecrab_agent_matrix`、`litecrab_session_restart`、`litecrab_ui_session_transport`、`litecrab_security_tests`、`litecrab_security_e2e`）。
- 直接验证：Registry 仍有 9 个 Tool；schema 是合法 JSON 且只包含 8 个默认启用 Tool；`shell` 不出现在 schema 中；直接调用 `shell` 返回 `CRAB_ERROR_TOOL_DISABLED (-1010)`、`toolExitCode == 0`、summary 为 `tool is disabled`；`do_shell` 未执行，marker 文件不存在。
- 合法已启用 Tool（`read`、`write`、`edit`、`grep`、`glob`、`csv_read`、`skill_read`、`skill_complete`）行为未发生变化。

## 4.3 第 2 步：收紧 Gateway 默认暴露范围

实施状态：**已完成（2026-09-02；2026-09-12 复核仍有效）**

原详细设计已归并到 [Security 模块架构设计](security_module_architecture.md) 的网络暴露控制章节。

### 实现功能

- 默认监听地址由 `0.0.0.0` 改为 `127.0.0.1`。
- 未完成认证能力前，非 loopback 地址需要显式启用开发兼容配置。
- 服务启动时明确记录 TCP 当前未认证，但日志不得记录凭证或敏感请求内容。
- 本步骤不宣称已经实现远程身份认证。

### 修改文件

- `include/litecrab/gateway.h`
- `src/config/config.c`
- `src/gateway/tcp_line.c`
- `src/main.c`
- `config/base_config.example.json`
- `README.md`
- `tests/test_security.c`
- `tests/test_main.c`
- `tests/test_security_e2e.py`

### 实施记录

- `RequestServerConfig` 新增 `allowUnauthenticatedRemote` 字段，只接受 `0`/`1`；`gateway.h` 包含 `<stddef.h>`。
- 新增统一绑定策略校验函数 `RequestServerValidateBindPolicy`，使用 `inet_pton` + `ntohl` 判定 `127.0.0.0/8` loopback 网段，非 loopback 且开关为 0 时拒绝。
- `LiteCrabConfigDefaults` 将 `listenIp` 改为 `127.0.0.1`、`allowUnauthenticatedRemote` 默认为 0。
- `load_base` 严格解析 `allow_unauthenticated_remote`，字段存在但不是 JSON boolean 时返回配置错误；`validate` 在端口和 Server limits 校验之前调用 `RequestServerValidateBindPolicy`，配置层 fail-closed。
- `StartRequestServer` 空 `listenIp` 回退到 `127.0.0.1`；在 `socket()` 之前再次调用 `RequestServerValidateBindPolicy`，直接调用 Gateway API 也无法绕过绑定策略。
- `main.c` 在 Agent Loop 启动后记录安全状态：loopback 模式记录 `exposure=loopback` 和 `userId is untrusted`；显式远程兼容模式记录 `WARNING unauthenticated remote TCP is explicitly enabled`；`[gateway] listening` 改为 `[gateway] starting`，避免在 `bind/listen` 成功前声称已监听。
- `config/base_config.example.json` 改为 `listen_ip: 127.0.0.1` 和 `allow_unauthenticated_remote: false`，示例不将兼容开关设为 true。
- `README.md` 新增 "TCP 暴露范围与身份限制" 段落，说明默认 loopback、`userId` 不可信、非 loopback 需显式开关、Transfer Station 跨设备部署注意事项。
- `tests/test_security.c` 新增 `test_bind_policy_matrix`，表驱动覆盖 18 个地址/开关组合（含 `127.0.0.0/8` 全段、wildcard、LAN、公网、非法字符串、`localhost`、空地址、非法开关值）。
- `tests/test_main.c` 新增 `test_gateway_bind_config`，验证默认值、`0.0.0.0` 无开关失败、`0.0.0.0` + true 成功、非 boolean 失败、字符串和整数 boolean 失败、非法 IPv4 失败、LAN 地址无开关失败、`127.0.0.2` 成功。
- `tests/test_security_e2e.py` 新增 `run_unsafe_remote_rejected`（进程退出码 2、stderr 含稳定关键词、端口未监听、stderr 不含 API Key）和 `run_explicit_remote_compat`（服务启动、非法 Session 拒绝、日志含 WARNING、日志不含 API Key 和 userId）。
- 不增加环境变量和命令行覆盖；兼容开关只能来自 Base Config。`include/litecrab/config.h` 无需直接修改（`LiteCrabAppConfig` 内嵌 `RequestServerConfig`，新字段自动进入 App Config）。

### 验收结果

使用全新 WSL 构建目录 `build-step2` 完成配置、编译和测试：

- `litecrab_security_tests`：85 项 check 全部通过，新增 32 项 bind policy 矩阵 check。
- `litecrab_tests`：通过，新增 `test_gateway_bind_config` 覆盖配置层默认值和 fail-closed。
- `litecrab_tcp_e2e`：通过，覆盖默认配置下 loopback 客户端完整 Agent 流程。
- `litecrab_security_e2e`：通过，新增 unsafe remote 拒绝（退出码 2、端口未监听）和显式兼容启动（WARNING 日志、API Key 不泄露）两组场景。
- 全量 8 项测试全部通过。
- 直接验证：缺省配置 `listenIp=127.0.0.1`、`allowUnauthenticatedRemote=0`；未授权 `0.0.0.0` 在配置阶段 fail-closed；直接调用 `StartRequestServer` 也无法绕过绑定策略（socket 创建前拒绝）；显式 `true` 可保留兼容启动；风险开关只接受 JSON boolean；安全日志说明 TCP 未认证和 `userId` 不可信；日志和 stderr 不包含测试 API Key 与请求 userId。

## 4.4 第 3 步：Trace 最小化与文件权限

实施状态：**部分实现（2026-09-12 复核）**

当前已实现：默认不记录完整 LLM messages、单字段有界截断、日志/Trace 轮转和批量 flush。

尚未满足：`trace_start.input`、Tool input/output、span input/output 和 `trace_end.finalOutput` 仍保存原文；没有统一摘要/hash/secret redaction；没有 `redacted/full/off` 配置；目录和文件权限未按本步骤强制为 `0700/0600`。

原详细设计已归并到 [Security 模块架构设计](security_module_architecture.md) 的 Trace 与 Secret 风险章节。

### 实现功能

- 默认 Trace 只记录摘要、长度和 hash，不记录原始用户输入、Tool 参数/输出和最终回答。
- 提供显式的 `redacted` 与 `full` 模式，默认使用 `redacted`。
- 日志目录权限收紧为 `0700`，日志和 Trace 文件收紧为 `0600`。
- 对 API Key、Authorization 和常见 secret 字段进行统一脱敏。

### 修改文件

- `include/litecrab/observability.h`
- `include/litecrab/config.h`
- `src/observability/observability.c`
- `src/config/config.c`
- `src/main.c`
- `config/base_config.example.json`
- `tests/test_security.c`

### 验收标准

- 使用 canary secret 发起请求后，所有日志和 Trace 中均不存在 secret 原文。
- `redacted` 模式不保存请求和响应全文。
- 日志目录、日志文件和 Trace 文件权限正确。
- `full` 模式必须通过显式配置才能启用。

## 4.5 第 4 步：Agent Run 与 Tool 调用预算

实施状态：**部分实现（2026-09-12 复核）**

当前已实现：固定 16 轮上限；normal/alarm 分别限制 Tool 调用为 16/24、LLM 调用为 8/12；请求 deadline 会传入 LLM，Tool 执行前也会检查取消和超时。

尚未满足：预算没有进入统一配置；16 轮仍是编译期常量；超限结果没有稳定的 `budget_exceeded` reason code；预算状态没有形成统一结构。

### 实现功能

- 将当前编译期 Tool 循环上限变为有界配置。
- 增加每次请求最大 Tool call 数。
- 增加每次 Run 总耗时上限。
- 超限后立即停止继续调用 LLM 和 Tool。
- 返回稳定的 `budget_exceeded` reason code。

### 修改文件

- `include/litecrab/kernel.h`
- `include/litecrab/config.h`
- `src/kernel/kernel.c`
- `src/config/config.c`
- `src/main.c`
- `config/base_config.example.json`
- `tests/test_security.c`
- `tests/test_agent_matrix.py`

### 验收标准

- 构造持续请求 Tool 的模型响应，达到预算后执行立即停止。
- 超限后不再发生额外 Tool 副作用。
- 返回稳定 reason code。
- 正常短任务不受影响。

## 4.6 第 5 步：Gateway 资源预算

实施状态：**部分实现，主体已落地（2026-09-12 复核）**

当前已实现：固定 Worker Pool、pending 队列、连接总量上限、16 KiB 请求上限、收发 timeout、满载快速返回 `server busy`、线程创建失败时逆序回收，以及停机时 shutdown/join。

尚未满足：没有单连接请求次数上限；当前 `SO_RCVTIMEO` 是单次 `recv` 空闲限制，持续缓慢发送字节仍可能长期占用 Worker，因此“慢连接不会无限占用线程”的验收条件尚未完全成立。

### 实现功能

- 限制同时活动连接数。
- 限制单连接连续请求数或最大空闲时间。
- 拒绝慢速和超长请求。
- 线程创建失败或连接容量耗尽时 fail-closed。
- 在可信 Principal 建立前，不使用客户端自报 `userId` 进行限流。

### 修改文件

- `include/litecrab/gateway.h`
- `include/litecrab/config.h`
- `src/gateway/tcp_line.c`
- `src/config/config.c`
- `config/base_config.example.json`
- `tests/test_security_e2e.py`
- `tests/test_tcp_e2e.py`

### 验收标准

- 超过连接上限时快速拒绝。
- 连接释放后容量恢复。
- 慢连接不会无限占用线程。
- 正常请求不受影响。

## 4.7 第 6 步：引入可信安全上下文

实施状态：**未实现（2026-09-12 复核）**

当前没有 `include/litecrab/security.h`、`src/security/context.c`、`CrabPrincipal` 或 `CrabSecurityContext`。Session owner 仍绑定客户端自报的 `userId`。

### 实现功能

- 定义 `CrabPrincipal` 和 `CrabSecurityContext`。
- Gateway 生成 Principal；客户端 `userId` 只能作为非安全显示字段，不能覆盖 Principal。
- Principal 随消息进入 Kernel、Session 和 Tool 调用链。
- 所有高风险入口要求存在有效的安全上下文。
- 当前 loopback TCP 可先映射为固定的低可信 `local-unauthenticated` Principal，并明确限制其能力。

### 修改文件

- 新增 `include/litecrab/security.h`
- 新增 `src/security/context.c`
- `include/litecrab/gateway.h`
- `include/litecrab/hub.h`
- `include/litecrab/session.h`
- `src/gateway/tcp_line.c`
- `src/hub/hub.c`
- `src/kernel/kernel.c`
- `src/session/session.c`
- `CMakeLists.txt`
- `tests/test_security.c`
- `tests/test_session_restart.py`

### 验收标准

- 修改请求体中的 `userId` 不会改变安全 Principal。
- Session 所有权按 Principal 绑定。
- 缺失或无效安全上下文不能执行高风险动作。
- Session 恢复时重新验证 Principal。

## 4.8 第 7 步：最小 Policy Engine 与唯一安全执行入口

实施状态：**未实现（2026-09-12 复核）**

Runtime 已有 enabled、readonly、schema 和 workspace 检查，但它们是局部 admission control，不构成基于 Principal/Profile/Capability/Resource 的统一 Policy Engine，也没有 `REQUIRE_APPROVAL` 和 policy revision。

### 实现功能

- 定义 `ALLOW`、`DENY` 和 `REQUIRE_APPROVAL` 三种决策。
- 决策包含稳定 reason code、rule ID、risk 和 policy revision。
- 建立唯一的安全 Tool 执行入口。
- Tool 暴露和 Tool 执行使用同一套决策规则。
- 审批模块完成前，`REQUIRE_APPROVAL` 按 DENY 处理。
- 旧的无上下文执行入口不得绕过策略。

首版 Policy 只处理以下因素：

- Principal 是否有效；
- Tool 是否启用；
- Tool 是否只读；
- Tool risk 和 permissions；
- 当前安全 profile。

### 修改文件

- `include/litecrab/security.h`
- 新增 `src/security/policy.c`
- `include/litecrab/runtime.h`
- `src/runtime/runtime.c`
- `src/kernel/kernel.c`
- `CMakeLists.txt`
- `tests/test_security.c`

### 验收标准

- 使用表驱动测试覆盖 Tool × profile × readonly × Principal。
- Tool 暴露决策和执行决策一致。
- 未匹配、未知值和缺少安全上下文全部拒绝。
- 无法通过直接调用旧 API 绕过策略。

## 4.9 第 8 步：安全 Profile 与严格配置

实施状态：**未实现（2026-09-12 复核）**

当前 `LiteCrabAppConfig` 没有 security 配置块，不存在 safe/standard/developer profile、默认 deny 或 policy revision。

### 实现功能

建议提供三个含义明确的 profile：

- `safe`：只读、无 shell、无 Tool 网络。
- `standard`：允许受策略约束的写入，高风险动作进入审批。
- `developer`：可显式开启更多能力，但仍不能绕过审计、预算和执行限制。

配置同时满足：

- 拒绝未知 security key；
- 拒绝非法 enum、越界数值和重复规则；
- 每份策略具有 revision；
- 不提供含糊的全局 `disable_security` 开关。

### 修改文件

- `include/litecrab/config.h`
- `include/litecrab/security.h`
- `src/config/config.c`
- `src/security/policy.c`
- `src/main.c`
- `config/base_config.example.json`
- `tests/test_security.c`

### 验收标准

- 缺省 profile 在本步骤实施前单独评审确认。
- security 配置错误时服务拒绝启动。
- `developer` 必须显式配置。
- policy revision 进入决策结果和安全审计。

## 4.10 第 9 步：安全文件访问

实施状态：**部分实现，仅 Skill 读取链较完整（2026-09-12 复核）**

当前已实现：`skill_read` 打开 package root 后使用 `openat + O_NOFOLLOW` 逐级读取；普通文件 Tool 会做 workspace 规范化和 `realpath` 边界检查。

尚未满足：普通 read/write/edit/grep/glob 没有统一 root fd 解析；检查和打开之间仍有 TOCTOU；缺少 hardlink、FIFO/device/socket、protected path、`fsync + renameat` 和统一新文件 `0600` 控制。

### 实现功能

- 启动时打开 workspace 根目录 fd。
- 每一级路径使用 `openat` 和 `O_NOFOLLOW` 解析。
- 只接受策略允许的普通文件和目录。
- 拒绝 symlink、FIFO、device、socket 和危险 hardlink。
- 写入采用同目录临时文件、`fsync` 和 `renameat`。
- 新文件默认权限为 `0600`。
- 增加 `.crab`、配置、凭证和启动脚本等 protected path。

### 修改文件

- 新增 `src/security/path.c`
- 扩展 `include/litecrab/security.h`
- `include/litecrab/runtime.h`
- `src/runtime/runtime.c`
- `src/runtime/builtin_tools.c`
- `CMakeLists.txt`
- `tests/test_security.c`

### 验收标准

覆盖并拒绝以下情况：

- `../` 和绝对路径逃逸；
- symlink 及 symlink swap；
- 危险 hardlink；
- FIFO、device 和 socket；
- 不安全的深层目录新建；
- protected path 访问；
- 原子写入中途失败。

## 4.11 第 10 步：持久化审批存储

实施状态：**未实现（2026-09-12 复核）**

Skill 的 Suspend Queue 是内存中的任务恢复结构，不是审批存储；当前没有与 Principal、参数 hash、policy revision 和过期时间绑定的审批记录。

### 实现功能

本步骤只实现审批数据层，暂不接入 Agent 对话循环：

- interruption 创建、读取和状态转换；
- 审批绑定 Principal、Session、Run、ToolCall、规范化参数 hash 和 policy revision；
- 支持过期、approve、reject 和 edit；
- 使用 `0600` 文件和有界 JSONL；
- 防止跨 Session、跨参数和重复审批。

### 修改文件

- 新增 `src/security/approval.c`
- `include/litecrab/security.h`
- 仅在复用安全落盘能力时调整 `src/session/session.c`
- `CMakeLists.txt`
- `tests/test_security.c`

### 验收标准

- 重启后 pending 审批可恢复。
- 过期审批不可使用。
- 参数变化后旧审批不可使用。
- 跨 Principal、Session 或 ToolCall 重放被拒绝。
- 存储损坏时 fail-closed。

## 4.12 第 11 步：审批接入 Agent 执行循环

实施状态：**未实现（2026-09-12 复核）**

现有 `replyToRunId/replyToInterruptId/correlationToken` 可用于 Skill 恢复，但没有 Policy 产生的审批中断、批准/拒绝/编辑协议，也没有恢复前重新决策。

### 实现功能

- Policy 返回 `REQUIRE_APPROVAL` 时暂停当前 Run。
- Gateway 返回结构化 interruption。
- 用户可以批准、拒绝或修改后恢复。
- 恢复前重新执行 Policy。
- 普通自然语言中的“是”不能自动放行。
- 一次审批只允许一个具体动作，不形成永久授权。

### 修改文件

- `include/litecrab/kernel.h`
- `include/litecrab/gateway.h`
- `src/kernel/kernel.c`
- `src/gateway/tcp_line.c`
- `src/security/approval.c`
- `src/security/policy.c`
- `src/session/session.c`
- `tests/test_security.c`
- `tests/test_security_e2e.py`
- 需要显示审批状态时调整 `others/UI/app.py`

### 验收标准

- 写入或执行动作可以暂停、批准、拒绝和恢复。
- 未批准前没有发生副作用。
- 服务重启后仍可恢复 pending interruption。
- 审批后参数或策略发生变化时重新拒绝。
- 审批不能跨调用重放。

## 4.13 第 12 步：受限进程执行，逐步替代 shell

实施状态：**部分实现，资源限制主体已落地（2026-09-12 复核）**

当前已实现：`exec_program` 使用 argv/execve 而非 shell；只解析 workspace 内真实可执行文件；子进程使用最小环境、独立进程组、固定 cwd、CPU/AS/NOFILE/FSIZE rlimit、wall timeout、TERM/KILL、waitpid 和 256 KiB 输出上限；原 `shell` 默认禁用。

尚未满足：入口不是固定 program ID/allowlist，调用参数仍可选择 workspace 内任意可执行文件；未接入 Principal/Policy；没有独立 `src/security/process.c` Helper；没有可靠进程数隔离，`RLIMIT_NPROC` 被明确跳过。

### 实现功能

- 新增“固定 program ID + typed argv”执行方式。
- 清理子进程环境变量和继承 fd。
- 固定允许的 cwd。
- 使用独立 process group。
- 设置 CPU、内存、文件大小、进程数和 fd 上限。
- timeout 后终止整个进程组。
- stdout 和 stderr 大小有界。
- 原始 `sh -c` shell 继续保持默认禁用。

### 修改文件

- 新增 `src/security/process.c`
- `include/litecrab/security.h`
- `include/litecrab/runtime.h`
- `src/runtime/builtin_tools.c`
- `src/runtime/runtime.c`
- `src/security/policy.c`
- `config/base_config.example.json`
- `CMakeLists.txt`
- `tests/test_security.c`
- `tests/test_security_e2e.py`

### 验收标准

以下攻击或失控行为均被限制：

- 无限 sleep；
- 无限输出；
- 读取 Agent secret 环境变量；
- 继承敏感 fd；
- fork bomb；
- CPU、内存和文件大小超限；
- timeout 后遗留子进程。

## 5. 后续增强计划

下列能力依赖前面的安全上下文、Policy 和受控执行链，不应提前与前述步骤并行修改。

### 5.1 Input、Tool Output 与 Final Output Guardrail

实施状态：**未实现**。现有大小截断和 PLC 字段过滤不是统一安全 Guardrail。

**功能：** 为外部内容增加 provenance 和数据标签；对进入模型的 Tool 输出及最终输出执行同步检查；恶意数据不能改变 capability 或审批结果。

**预计文件：**

- 新增 `src/security/guardrail.c`
- `include/litecrab/security.h`
- `src/kernel/kernel.c`
- `tests/test_security.c`
- `tests/test_security_e2e.py`

### 5.2 Secret Broker

实施状态：**未实现**。当前只有 `apiKeyEnv`、Alarm 凭据复制清理和 `exec_program` 最小环境等局部基础。

**功能：** 使用 secret handle 代替在 Tool 参数中传递原文；只对指定 Tool 和目标主机短时提供 secret；子进程不再继承 Agent 完整环境变量。

**预计文件：**

- 新增 `src/security/secret.c`
- `include/litecrab/security.h`
- `src/kernel/llm.c`
- `src/runtime/runtime.c`
- `src/main.c`
- `tests/test_security.c`

### 5.3 Tool 网络出口控制

实施状态：**未实现**。当前子进程继承进程网络能力，没有 Tool 级 host/port/IP allowlist。

**功能：** 将 LLM Provider 网络出口与 Tool 网络出口分离；Tool 默认无网络；按 host、port 和解析后的 IP 执行 allowlist，并检查 redirect 和 DNS 重绑定。

**预计文件：**

- 新增 `src/security/network.c`
- `include/litecrab/security.h`
- `src/kernel/llm.c`
- 未来新增的网络 Tool
- `tests/test_security_e2e.py`

### 5.4 Skill 与插件供应链

实施状态：**部分实现**。当前已有 Definition hash、同名/格式校验和 canonical package path，但没有签名、trust tier、requested capability、版本 pinning 和完整包验证。

**功能：** 增加严格 manifest、内容 hash、trust tier、requested capabilities、命名空间、防同名覆盖及安装/升级授权；Skill 只能请求能力，不能授予能力。

**预计文件：**

- `src/kernel/skill.c`
- `include/litecrab/kernel.h`
- `include/litecrab/security.h`
- `src/security/policy.c`
- `tests/test_security.c`

### 5.5 可选 S2 内核沙箱

实施状态：**未实现**。当前 `exec_program` 只有用户态 rlimit、进程组和超时，没有 namespace、seccomp 或 cgroup。

**功能：** 在运行时能力探测通过时，为受限执行器增加 namespace、seccomp 和 cgroup。能力不可用时减少可执行能力，不能静默退化为无限制执行。

**预计文件：**

- 扩展 `src/security/process.c`
- `include/litecrab/security.h`
- `src/security/policy.c`
- `src/main.c`
- `tests/test_security_e2e.py`

## 6. 每项实施流程

每个计划项按以下固定流程执行：

```text
讨论目标与威胁
      |
确认默认行为和兼容性
      |
先增加失败测试
      |
实施最小代码修改
      |
单元测试 -> 相关 E2E -> 全量回归
      |
检查动作未发生、reason code、脱敏审计
      |
用户评审
      |
形成独立提交点后再进入下一项
```

如果实施过程中发现当前项必须依赖尚未确认的下一项，应暂停实现并重新拆分计划，而不是扩大本次修改范围。

## 7. 当前推荐实施顺序

根据 2026-09-12 的代码复核，不再从已完成的 Step 1 开始。建议顺序调整为：

1. **先完成 Step 3**：当前 Trace 仍保存用户、Tool 和最终输出原文，且权限未被代码强制。
2. **完成 Step 4 和 Step 5 的剩余验收项**：把预算配置化、增加稳定 reason code，并封堵持续慢速连接长期占用 Worker。
3. **依次实施 Step 6、7、8**：先建立可信 Principal/ExecutionContext，再建立统一 Policy，最后提供严格 Profile；不能只靠 Tool 元数据宣称安全。
4. **完成 Step 9**：把普通文件 Tool 迁移到统一安全路径 API。
5. **实施 Step 10、11**：审批存储和 Agent 审批恢复必须成对完成。
6. **补齐 Step 12**：在现有 `exec_program` 基础上增加固定 program registry、Policy 集成和更强进程隔离。
7. **新增 Mutation Ledger 计划项**：以“问题类型 + 原因 + 设备 + 参数”作为副作用幂等键，解决不同告警 occurrence 对同一参数重复修改的问题。

每完成一项，仍按“失败测试 → 最小实现 → 相关 E2E → 全量回归 → 独立提交点”的流程验收。
