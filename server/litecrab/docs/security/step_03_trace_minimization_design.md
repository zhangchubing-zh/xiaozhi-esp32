# 第 3 步实施设计：Trace 最小化与文件权限

状态：设计完成，待评审确认后实施  
对应计划：[安全能力渐进式实施计划](security_incremental_implementation_plan.md#44-第-3-步trace-最小化与文件权限)  
设计基线：2026-09-02

## 1. 本步骤目标

将 LiteCrab 的可观测性从“默认记录原始业务载荷”改为“默认只记录运行元数据和不可逆摘要”，并确保日志文件在创建、重开和并发写入时保持最小文件权限。

本步骤完成后应满足：

1. 默认 `redacted` 模式不写入原始用户输入、Tool 输入/输出、LLM 输出、Span 输入/输出和最终回答。
2. 默认模式保留排障所需的长度、SHA-256、状态、耗时、计数、Tool 名称和 trace/span 关联信息。
3. 显式 `full` 模式可以记录业务载荷，但 API Key、Authorization、Bearer Token、Cookie、Password 等秘密仍必须脱敏。
4. `litecrab.log` 和 `agent_trace_*.jsonl` 共用同一套秘密脱敏逻辑。
5. 日志目录权限为 `0700`，日志和 Trace 文件权限为 `0600`。
6. 日志文件使用安全的 fd 打开方式，拒绝符号链接、非普通文件和危险 hardlink。
7. Trace Viewer 在默认模式下仍可按 Session、Trace、Tool 和状态进行浏览，只是不再显示业务原文。

## 2. 安全边界与非目标

### 2.1 本步骤处理的数据出口

```text
AgentTraceStart          用户输入、userId
AgentTraceFinish         最终回答
AgentTraceStartSpan      Span 输入、metadata
AgentTraceEndSpan        Span 输出
AgentTraceLogTool        Tool 参数、Tool 输出、错误详情
AgentTraceLogLlm         LLM 文本输出
AgentTraceLogAnomaly     异常详情
AgentTraceLogMessage     结构化事件消息
LogPrint                 litecrab.log 文本及复制到 JSONL 的 log 事件
LITECRAB_DEBUG_LLM       Tool call 完整参数
```

只修改部分 `AgentTrace*` 函数不能完成本步骤，因为 `LogPrint` 当前先把原始消息写入 `litecrab.log`，随后又复制到 JSONL；`src/kernel/llm.c` 的 Debug 路径还会把完整 Tool 参数传给 `LogPrint`。

### 2.2 本步骤不包含

- 不实现日志轮转、总字节预算和磁盘满保留策略。
- 不实现审计事件的 sequence、prevHash、MAC 或抗篡改链。
- 不实现设备 KMS、Secret Broker 或凭证动态租约。
- 不对 Session 快照、Working Memory 和 Artifact 做静态加密。
- 不承诺 SHA-256 能保护低熵输入免受字典猜测。
- 不实现按 Principal 的日志访问控制；可信 Principal 尚未建立。
- 不修改 Agent 的业务回答或发送给 LLM 的上下文。
- 不把 Trace Viewer 端的 JavaScript 遮罩视为安全边界。

日志轮转、抗篡改审计、Secret Broker 和数据分级应在后续步骤独立实施。

## 3. 当前代码状态

### 3.1 当前 Trace 明文内容

`src/observability/observability.c` 当前写入：

| 事件 | 当前原文字段 |
|---|---|
| `trace_start` | `userId`、`sessionId`、`input` |
| `trace_end` | `finalOutput` |
| `span_start` | `input`、`metadata` |
| `span_end` | `output` |
| `tool` | `input`、最多 16 KiB `output` |
| `llm` | 最多 4 KiB `output` |
| `anomaly` | `detail`，Tool 错误时可能重复记录 Tool 输出 |
| `log` | `message`，内容来自任意 `LogPrint` 调用 |

这些字段可能包含文件内容、命令参数、模型回答、用户隐私和凭证。

### 3.2 当前文本日志旁路

`LogPrint` 的当前顺序为：

```text
格式化消息
    |
原文写入 litecrab.log
    |
AgentTraceLogMessage
    |
原文再次写入 agent_trace_*.jsonl
```

即使只修改 `AgentTraceLogMessage`，`litecrab.log` 仍会泄露原文。秘密脱敏必须发生在写入任一文件之前。

### 3.3 当前 LLM Debug 旁路

当设置 `LITECRAB_DEBUG_LLM` 时，`src/kernel/llm.c` 当前记录：

```text
[llm_debug] call=<n> name=<tool> input=<完整 Tool 参数 JSON>
```

这条路径可以绕过 Tool Trace 的默认载荷最小化，必须同步修改。

### 3.4 当前文件创建方式

- 日志目录通过 `mkdir(..., 0775)` 创建。
- `litecrab.log` 使用 `fopen(..., "a")`，实际权限依赖进程 umask。
- Trace 先用 `access` 查重，再使用 `fopen(..., "a")`，存在检查与打开之间的竞争窗口。
- 没有显式拒绝符号链接、非普通文件或 hardlink。
- 文件描述符没有显式 `O_CLOEXEC`，可能被子进程继承。

### 3.5 当前配置

`log` 对象目前只解析 `dir`，历史 `filename` 仅用于兼容提取目录。没有 Trace payload 模式。

### 3.6 Trace Viewer 依赖

`others/trace_viewer/viewer.js` 使用以下字段组织页面：

- `traceId`、`rootSpanId`、`sessionId`；
- `input`、`finalOutput`；
- `toolName`、Tool `input/output`；
- `status`、`durationMs`、Token 和调用计数。

Viewer 已有客户端 `redactSecrets`，但它发生在敏感数据读取到浏览器之后，不能替代写盘前脱敏。

## 4. Trace 模式契约

### 4.1 配置项

在 Base Config 的 `log` 对象增加：

```json
{
  "log": {
    "dir": "logs",
    "trace_payload": "redacted"
  }
}
```

允许值：

| 值 | 是否默认 | 行为 |
|---|---:|---|
| `redacted` | 是 | 业务载荷只记录 marker、字节数和 SHA-256 |
| `full` | 否 | 记录经过 secret redaction 的业务载荷 |

未知字符串、非字符串值和空字符串均导致配置加载失败。不存在 `off` 或 `unsafe` 模式。

### 4.2 模式不影响秘密脱敏

`full` 的含义是“允许保存经脱敏的业务载荷”，不是“允许保存秘密原文”。以下数据在两种模式中都必须被替换：

- 当前 LLM API Key 的精确值；
- `Authorization` 字段值；
- `Bearer <token>`；
- JSON 或 `key=value` 中的 `token`、`apiKey`、`api_key`、`password`、`cookie`；
- `access_token`、`refresh_token`、`client_secret`、`secret` 等常见凭证字段。

### 4.3 默认模式字段格式

为了兼容现有 Viewer，原字符串字段保留，但其值固定为 marker，同时增加摘要字段：

```json
{
  "input": "[redacted]",
  "inputBytes": 123,
  "inputSha256": "sha256:<64 hex>"
}
```

同样适用于：

- `finalOutput`；
- Span `input`、`metadata`、`output`；
- Tool `input`、`output`；
- LLM `output`；
- Anomaly `detail`。

空字符串约定：

```text
payload == NULL 或 payload == ""
    marker       = ""
    payloadBytes = 0
    payloadSha256= "sha256:<empty string digest>"
```

这样可以区分“没有内容”和“有内容但已隐藏”。

### 4.4 保留的运行元数据

默认模式仍可记录：

- `traceId`、`rootSpanId`、`spanId`、`parentSpanId`；
- `sessionId`，用于现有 Session/Trace 关联；
- Tool 名称、Span 类型/名称、模型名称；
- 状态、错误码、Tool 名称列表；
- 时间、耗时、Token 数、LLM/Tool 调用计数；
- 安全模式名称。

`userId` 当前由客户端自报且可能包含个人标识。默认模式应将其作为 payload：保留 `userId: "[redacted]"`，增加 `userIdBytes` 和 `userIdSha256`。在可信 Principal 引入前，不依赖该 hash 做授权或审计主体判断。

## 5. 摘要与脱敏设计

### 5.1 SHA-256 摘要

使用项目已经链接的 OpenSSL `EVP_sha256` 计算 payload SHA-256，输出固定格式：

```text
sha256:<64 个小写十六进制字符>
```

用途：

- 判断两次事件是否引用相同载荷；
- 在不保存原文时辅助定位重复问题；
- 测试写盘内容与输入之间的确定性关系。

限制：

- SHA-256 不是加密。
- 对短口令、布尔值和其他低熵内容，攻击者可能通过字典计算猜测原文。
- 后续有设备密钥时，可评估改用 HMAC；本步骤不伪装具备该能力。

### 5.2 统一文本脱敏

在 Observability 模块实现统一函数：

```c
int CrabObservabilityRedactText(const char* input, char* output, size_t outputSize);
```

处理顺序：

1. 替换初始化时注册的已知 secret 精确值。
2. 替换不区分大小写的 `Bearer <token>`。
3. 替换 JSON 字符串中的敏感 key 值。
4. 替换 `key=value` 和 `key: value` 形式的敏感值。
5. 保证输出始终 NUL 终止；空间不足时返回截断状态，不回退为原文。

失败策略：

- redaction 缓冲区不足时写入已脱敏的截断内容，并追加安全截断 marker。
- 内部错误时使用固定文本 `[redaction-error]`，禁止写入原文作为 fallback。

### 5.3 已知 secret 注册

模式匹配无法发现脱离字段名出现的任意 API Key。初始化配置需要允许注册少量已知秘密：

```c
#define CRAB_OBSERVABILITY_MAX_SECRETS 8

typedef struct {
    const char* directory;
    CrabTracePayloadMode payloadMode;
    const char* knownSecrets[CRAB_OBSERVABILITY_MAX_SECRETS];
    size_t knownSecretCount;
} CrabObservabilityConfig;
```

`main.c` 至少传入当前 `app.llm.apiKey`。Observability 初始化时复制有界值到内部内存；关闭时显式清零内部 secret 缓冲区。

要求：

- 空值不注册。
- 太短的值不做全局精确替换，避免把普通文本大量误删；建议最小长度 8。
- 超长或超量 secret 导致初始化失败，而不是静默忽略。
- 已知 secret 绝不写入错误消息或启动日志。

## 6. Observability API 设计

### 6.1 新增类型

文件：`include/litecrab/observability.h`

```c
typedef enum {
    CRAB_TRACE_PAYLOAD_REDACTED = 0,
    CRAB_TRACE_PAYLOAD_FULL = 1
} CrabTracePayloadMode;

typedef struct {
    const char* directory;
    CrabTracePayloadMode payloadMode;
    const char* knownSecrets[CRAB_OBSERVABILITY_MAX_SECRETS];
    size_t knownSecretCount;
} CrabObservabilityConfig;
```

### 6.2 新增初始化入口

```c
int InitLogWithConfig(const CrabObservabilityConfig* config);
```

兼容入口保留：

```text
InitLog()          -> logs + redacted
InitLogWithDir(x) -> x + redacted
```

现有调用方不会因为新增模式而默认进入 full。

### 6.3 查询与摘要 API

为 Debug 日志和测试提供：

```c
CrabTracePayloadMode CrabObservabilityGetPayloadMode(void);
int CrabObservabilityDescribePayload(const char* payload,
                                     size_t* bytes,
                                     char sha256[72]);
```

`DescribePayload` 只输出长度和 hash，不返回 payload 内容，可供 `src/kernel/llm.c` 的 Debug 路径使用。

### 6.4 现有 Trace API 保持签名

`AgentTraceStart`、`AgentTraceFinish`、`AgentTraceLogTool` 等现有函数签名暂不修改。它们读取初始化时设置的全局 Observability 配置，并在内部决定写 marker/hash 还是脱敏后的 full payload。

这可以避免本步骤同时修改大量 Kernel/Skill 调用点。

## 7. 安全文件创建

### 7.1 私有目录

用 `ensure_private_directory` 替换当前 `mkdirs(..., 0775)` 语义：

1. 新创建的路径组件使用 `0700`。
2. 最终日志目录必须是目录。
3. 最终日志目录使用 `chmod(..., 0700)` 收紧权限。
4. 不递归修改已经存在的父目录权限，避免影响 `/tmp`、workspace 或共享父目录。
5. 最终目录如果是符号链接则拒绝；本步骤不实现完整的逐级 `openat` 路径解析。

### 7.2 `litecrab.log`

使用：

```c
open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600)
```

打开后执行：

1. `fstat` 确认为普通文件。
2. 检查 link count 为 1；危险 hardlink 直接拒绝。
3. `fchmod(fd, 0600)`，收紧历史遗留的宽权限文件。
4. `fdopen(fd, "a")` 转换为现有 `FILE*`。

任一步失败都关闭 fd 并使日志初始化失败。

### 7.3 Trace 文件

删除 `access` 后 `fopen` 的查重方式，改为循环尝试：

```c
open(candidate,
     O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
     0600)
```

`EEXIST` 时尝试下一个编号，其他错误直接失败。打开后同样执行 `fstat`、link count 和 `fchmod` 检查，再 `fdopen`。

### 7.4 权限不依赖 umask

测试中应临时设置 `umask(0)`，证明目录和文件最终仍分别是 `0700` 和 `0600`。测试结束后恢复原 umask，避免污染其他测试。

## 8. 文件级修改说明

### 8.1 `include/litecrab/observability.h`

修改内容：

- 显式包含 `<stddef.h>`。
- 定义 `CrabTracePayloadMode`。
- 定义 `CrabObservabilityConfig` 和最大已知 secret 数。
- 声明 `InitLogWithConfig`。
- 声明 payload mode 查询、摘要和统一脱敏函数。
- 保留现有 API 作为默认 redacted 兼容入口。

### 8.2 `include/litecrab/config.h`

在 `LiteCrabAppConfig` 中增加：

```c
CrabTracePayloadMode tracePayloadMode;
```

虽然 `kernel.h` 当前间接包含 Observability 类型，`config.h` 应直接包含需要的声明，避免依赖脆弱的传递 include。

### 8.3 `src/config/config.c`

修改内容：

1. 默认 `tracePayloadMode = CRAB_TRACE_PAYLOAD_REDACTED`。
2. 在 `log` 对象中严格读取 `trace_payload`。
3. 仅接受 `redacted` 和 `full`。
4. 字段存在但类型错误、为空或未知时返回配置错误。
5. 错误文本不得包含其他配置值或 API Key。

建议错误：

```text
invalid log.trace_payload: expected redacted or full
```

### 8.4 `src/observability/observability.c`

这是本步骤的主要修改文件，包含以下子任务：

1. 保存当前 payload mode 和有界 known secret 集合。
2. 实现 SHA-256 描述函数。
3. 实现统一 secret redaction。
4. 实现 marker + bytes + sha256 字段写入辅助函数。
5. 所有 payload Trace 事件根据模式输出。
6. `LogPrint` 在写入文本日志前先脱敏。
7. `AgentTraceLogMessage` 对直接调用者再次执行脱敏，避免绕过 `LogPrint`。
8. 使用私有目录和安全 fd 打开日志/Trace 文件。
9. 关闭时清零 known secret 内存。

需要注意：`LogPrint -> AgentTraceLogMessage` 会经过两次脱敏。脱敏函数必须幂等，`[REDACTED]` 再次输入时不能继续变形。

### 8.5 `src/main.c`

将：

```c
InitLogWithDir(app.logDir)
```

替换为构造 `CrabObservabilityConfig` 后调用 `InitLogWithConfig`：

```text
directory   = app.logDir
payloadMode = app.tracePayloadMode
knownSecret = app.llm.apiKey（非空时）
```

初始化完成后记录当前模式：

```text
[security] trace_payload=redacted
```

如果模式为 `full`，记录明确警告：

```text
[security] WARNING trace payload logging is full; secrets remain redacted
```

日志中不得打印 known secret 的值、长度或 hash。

### 8.6 `src/kernel/llm.c`

修改 `LITECRAB_DEBUG_LLM` 路径：

默认 redacted 模式只记录：

```text
call index
Tool name
inputBytes
inputSha256
```

不再把完整 `r->calls[i].input` 交给 `LogPrint`。

full 模式如需记录参数，也必须先调用 `CrabObservabilityRedactText`，禁止依赖 `LogPrint` 作为唯一防线。推荐两种模式都只记录摘要，使 Debug 日志不成为 Trace 配置的旁路。

### 8.7 `src/kernel/kernel.c`

现有 `AgentTraceStart/Finish/Tool/Llm` 调用签名保持不变，因此主执行链预计不需要结构性修改。

需要复核所有传入字段已被 Observability 内部按 payload 处理，尤其是：

- `m.content`；
- `resp.calls[i].input`；
- Tool response；
- `resp.text`；
- `ans`。

若实现完全在 Observability 内部完成，该文件不应产生无意义 diff。

### 8.8 `src/kernel/skill.c`

Skill 事件大多通过 `LogPrint` 或 `AgentTraceLogMessage` 记录结构化元数据，统一脱敏后无需逐个改调用点。

必须验证 Skill Resolver 的 `resp.text` 经 `AgentTraceLogLlm` 后在 redacted 模式不落原文。预计不需要修改本文件。

### 8.9 `config/base_config.example.json`

加入安全默认：

```json
"log": {
  "dir": "logs",
  "level": "INFO",
  "trace_payload": "redacted"
}
```

示例不能使用 `full`。

### 8.10 `README.md`

说明：

- Trace 默认不保存业务输入/输出原文。
- `redacted` 模式保留长度和 SHA-256 以便关联排障。
- `full` 必须显式配置，仍会脱敏已知凭证。
- SHA-256 不是加密，低熵内容存在被猜测风险。
- 日志目录和文件权限要求。
- Trace Viewer 中出现 `[redacted]` 是预期行为。

### 8.11 `tests/test_security.c`

新增独立测试组：

#### A. 默认 redacted 模式

1. 设置 `umask(0)`。
2. 创建临时日志目录。
3. 使用 canary API Key 初始化 Observability。
4. 调用所有 payload Trace API，分别传入唯一 canary：
   - user input；
   - final output；
   - Tool input/output；
   - LLM output；
   - Span input/metadata/output；
   - anomaly detail；
   - direct log message。
5. 关闭日志并读取两个文件。
6. 断言所有 canary 原文不存在。
7. 断言 `[redacted]`、对应长度和 SHA-256 存在。
8. 断言 API Key、Bearer Token 和敏感 key 值不存在。

#### B. full 模式仍脱敏 secret

1. 使用 `full` 初始化。
2. 写入普通业务 canary 和 secret canary。
3. 断言普通业务 canary存在。
4. 断言 secret canary 不存在且 `[REDACTED]` 存在。

#### C. 文件权限

- 日志目录 mode 精确为 `0700`。
- `litecrab.log` mode 精确为 `0600`。
- Trace 文件 mode 精确为 `0600`。
- `umask(0)` 下仍满足以上要求。
- 预置 symlink、FIFO 或 hardlink 时初始化失败且目标未被写入。
- Trace 同名竞争通过 `O_EXCL` 选择新文件，不覆盖旧文件。

### 8.12 `tests/test_main.c`

配置测试增加：

- 缺省 mode 是 `CRAB_TRACE_PAYLOAD_REDACTED`。
- `trace_payload: "redacted"` 解析成功。
- `trace_payload: "full"` 解析成功。
- 未知字符串失败。
- boolean、整数和 null 类型失败。

### 8.13 `tests/test_security_e2e.py`

建议扩展为真实 Agent Trace canary 测试：

1. 启动本地 Mock Provider。
2. 用户请求包含唯一 input canary 和 secret canary。
3. Mock Provider 返回唯一 LLM output canary。
4. Mock Provider 再产生一个带 canary 参数的只读 Tool call。
5. Agent 完成后关闭服务并读取日志目录。
6. 默认模式断言所有业务 canary、API Key 和 Bearer Token均未落盘。
7. 断言 trace、状态、Tool 名称、长度和 hash仍存在。
8. 以 `full` 模式再启动一次，断言普通业务 canary可见而 secret canary仍不可见。

该 E2E 可以证明 Kernel、LLM、Tool 和文本日志路径没有遗漏，仅测试 Observability 单函数不足以证明完整数据流。

### 8.14 `others/trace_viewer/README.md`

更新字段说明：

- 默认 `input/output/finalOutput` 显示 `[redacted]`。
- 新增 `*Bytes` 和 `*Sha256` 字段。
- Viewer 中的客户端遮罩只是防御补充，不是写盘安全控制。
- 搜索功能在 redacted 模式只能搜索元数据、Tool 名称和 hash，不能搜索业务原文。

预计不需要立即修改 `viewer.js`：保留字符串 marker 可以维持当前渲染。可以在后续 UI 优化中显示长度和 hash，但不作为本步骤验收前置。

### 8.15 活跃文档同步

功能验收后更新：

- `docs/security/security_incremental_implementation_plan.md`：标记第 3 步完成并记录验证结果。
- `docs/security/security_architecture_design.md`：将 Trace 原文落盘从当前缺口更新为已完成的默认最小化控制，同时保留轮转和抗篡改缺口。

`docs/history/` 下的历史文档不修改。

## 9. 修改文件汇总

| 文件 | 类型 | 修改目的 |
|---|---|---|
| `include/litecrab/observability.h` | 生产代码 | 定义 payload mode、初始化配置、摘要与脱敏 API |
| `include/litecrab/config.h` | 生产代码 | 保存 Trace payload mode |
| `src/config/config.c` | 生产代码 | 默认 redacted、严格解析 `log.trace_payload` |
| `src/observability/observability.c` | 生产代码 | 最小化、SHA-256、secret redaction、安全文件创建 |
| `src/main.c` | 生产代码 | 传入模式和当前 API Key，记录安全模式 |
| `src/kernel/llm.c` | 生产代码 | 消除 Debug Tool 参数明文旁路 |
| `config/base_config.example.json` | 配置示例 | 设置安全默认模式 |
| `README.md` | 使用文档 | 说明模式、风险和权限 |
| `tests/test_security.c` | 安全测试 | 验证载荷不落盘、secret 脱敏和文件权限 |
| `tests/test_main.c` | 配置测试 | 验证默认值和严格枚举 |
| `tests/test_security_e2e.py` | 安全 E2E | 验证完整 Agent 数据流无明文旁路 |
| `others/trace_viewer/README.md` | 工具文档 | 说明新字段和默认显示行为 |
| `docs/security/security_incremental_implementation_plan.md` | 计划文档 | 完成后记录状态 |
| `docs/security/security_architecture_design.md` | 活跃设计 | 同步已落地能力和剩余缺口 |

预计不修改：

- `src/kernel/kernel.c`：现有 Trace API 签名保持不变。
- `src/kernel/skill.c`：统一写盘前脱敏覆盖其现有调用。
- `others/trace_viewer/viewer.js`：marker 字符串兼容现有渲染。
- `CMakeLists.txt`：第 0 步已有安全测试目标，OpenSSL 已链接到 `litecrab`。

## 10. 修改后的数据流总结

### 10.1 默认 redacted Trace

```text
用户输入 / Tool 参数 / Tool 输出 / LLM 输出 / 最终回答
    |
    v
AgentTrace* API
    |
    +--> payload bytes
    +--> SHA-256
    +--> 固定 marker "[redacted]"
    |
    v
JSONL event
    |
    +-- 关联 ID、状态、计数、耗时保留
    +-- 业务原文不写盘
```

### 10.2 full Trace

```text
业务 payload
    |
    v
known-secret exact replacement
    |
Bearer / JSON secret key / key=value redaction
    |
    +-- 失败时 [redaction-error]，不回退原文
    v
经脱敏的 full payload
    |
    v
JSONL event
```

### 10.3 文本日志

```text
LogPrint(fmt, ...)
    |
格式化到内存缓冲区
    |
统一 secret redaction
    |
    +--> litecrab.log
    |
    +--> AgentTraceLogMessage
             |
             +--> 二次幂等 redaction
             +--> agent_trace JSONL log event
```

### 10.4 LLM Debug

```text
LITECRAB_DEBUG_LLM enabled
    |
Tool call input
    |
CrabObservabilityDescribePayload
    |
call index + tool name + bytes + sha256
    |
LogPrint

完整 Tool 参数不进入日志
```

### 10.5 文件创建

```text
InitLogWithConfig
    |
ensure private final directory (0700)
    |
open litecrab.log (NOFOLLOW, CLOEXEC, 0600)
    |
fstat regular + link count == 1 + fchmod
    |
open unique trace (EXCL, NOFOLLOW, CLOEXEC, 0600)
    |
fstat + fchmod + fdopen
```

## 11. 测试矩阵

| 模式/层级 | 输入 | 预期写盘内容 | 禁止内容 |
|---|---|---|---|
| redacted trace_start | 用户输入 | marker、bytes、SHA-256 | 用户原文 |
| redacted trace_end | 最终回答 | marker、bytes、SHA-256 | 回答原文 |
| redacted tool | 参数和结果 | Tool 名、状态、摘要 | 参数/结果原文 |
| redacted llm | 模型输出 | model、Token、摘要 | 模型原文 |
| redacted span | input/meta/output | span 关系、摘要 | 载荷原文 |
| redacted anomaly | 错误详情 | category、level、摘要 | 错误原文 |
| full | 普通业务文本 | 经脱敏业务文本 | 已知 secret |
| full | Authorization/Bearer | `[REDACTED]` | Token 原文 |
| LogPrint | API Key 独立出现 | `[REDACTED]` | API Key 原文 |
| direct AgentTraceLogMessage | secret key=value | `[REDACTED]` | secret 原文 |
| LLM debug | Tool 参数 | name、bytes、SHA-256 | 参数原文 |
| 文件权限 | `umask(0)` | dir 0700、file 0600 | group/other 权限 |
| 日志 symlink | 预置链接 | 初始化失败 | 写入链接目标 |
| 日志 hardlink/FIFO | 非安全节点 | 初始化失败 | 写入目标 |
| Trace 同名 | 已存在文件 | 创建下一个编号 | 覆盖已有文件 |
| 配置 | 未配置 mode | redacted | full |
| 配置 | 未知/错误类型 | 启动失败 | 静默降级 |

## 12. 兼容性影响

### 12.1 明确的行为变化

- 默认 Trace Viewer 不再显示用户输入、LLM 输出、Tool 输入/输出和最终回答原文。
- 依赖 Trace 全文进行排障或离线分析的流程需要显式选择 `full`。
- 现有 `0775` 日志目录会被收紧为 `0700`。
- 现有宽权限 `litecrab.log` 会被收紧为 `0600`。
- 符号链接、hardlink 或非普通日志文件会导致初始化失败。
- `LITECRAB_DEBUG_LLM` 不再输出完整 Tool 参数。

### 12.2 保持兼容的部分

- Trace JSONL 事件类型不变。
- 现有字符串字段仍存在，只是在 redacted 模式为固定 marker。
- Trace/span/session 关联、耗时、Token 和调用计数仍可用。
- `InitLog` 和 `InitLogWithDir` 保留，默认行为变为 redacted。
- `AgentTrace*` 函数签名保持不变。

### 12.3 运维注意事项

- `full` 应只用于受控调试环境，并限制日志保留时间。
- `full` 不能保证识别所有未知业务秘密；已知 API Key 和常见凭证格式会强制脱敏。
- 如果外部日志采集进程依赖 group-readable 权限，实施前需要调整为同一用户运行或使用明确的受控导出机制，不能恢复 `0640/0644` 作为临时修复。

## 13. 实施顺序

建议顺序：

1. 在 `observability.h` 定义 mode、配置和辅助 API。
2. 在 `observability.c` 实现 SHA-256 和统一 secret redaction，并先增加纯函数测试。
3. 实现安全目录和安全文件打开，增加 `umask(0)`、symlink、hardlink、FIFO 测试。
4. 改造各 `AgentTrace*` payload 字段，增加 redacted/full 单元测试。
5. 改造 `LogPrint`，确保文本日志与 JSONL 都在写盘前脱敏。
6. 修改 `src/kernel/llm.c`，关闭 Debug 参数原文旁路。
7. 在 Config 中增加 `log.trace_payload` 严格解析。
8. 修改 `main.c` 使用 `InitLogWithConfig` 并注册当前 API Key。
9. 更新示例配置、README 和 Trace Viewer 文档。
10. 扩展安全 E2E，覆盖完整 Agent canary 数据流。
11. 运行独立安全测试、Agent E2E 和全量回归。
12. 验收通过后同步总计划和活跃安全架构文档。

## 14. 验证命令与通过标准

使用全新或重新配置的构建目录：

```sh
cmake -S . -B build-step3 -DLITECRAB_ENABLE_STATIC_LINK=OFF
cmake --build build-step3 -j2
ctest --test-dir build-step3 -R litecrab_security --output-on-failure
ctest --test-dir build-step3 -R 'litecrab_(tcp_e2e|agent_matrix)' --output-on-failure
ctest --test-dir build-step3 --output-on-failure
```

通过标准：

1. 默认模式为 `redacted`。
2. 所有用户、Tool、LLM、Span、Anomaly 和 Final Output canary 原文均不落盘。
3. 默认模式保留对应 bytes 和 SHA-256。
4. `full` 模式保留普通业务 canary，但不保留任何 secret canary。
5. 当前 LLM API Key 即使脱离字段名出现在日志消息中也会被替换。
6. `LITECRAB_DEBUG_LLM` 不记录完整 Tool 参数。
7. 日志目录权限精确为 `0700`，文件权限精确为 `0600`。
8. symlink、hardlink、FIFO 等危险日志目标被拒绝且没有副作用。
9. Trace Viewer 能加载新 Trace，不出现解析错误。
10. 全量测试通过。
11. `git diff --check` 无错误。

## 15. 回退边界

本步骤不修改 Session、Tool 或 TCP 协议，但会改变 Trace 内容契约和日志权限。代码、配置、测试和文档应形成一个独立提交整体回退。

不能部分回退：

- 只回退 Trace payload 处理会重新泄露 JSONL 原文。
- 只保留 Trace 处理而回退 `LogPrint` 会从 `litecrab.log` 泄露。
- 只保留 redaction 而回退 LLM Debug 修改会重新出现 Tool 参数旁路。
- 只改创建 mode 而不 `fchmod`，旧文件仍可能保持宽权限。
- 只在 Viewer 中遮罩不能保护已经写盘或被采集的原文。

如果必须恢复全文排障，应使用显式 `trace_payload: full`，不能回退默认安全模式。

## 16. 评审重点

实施前建议确认：

1. 是否接受默认模式保留 `sessionId` 用于关联，但对当前不可信 `userId` 只保存 marker、长度和 hash。
2. 是否接受 `full` 模式仍强制脱敏 API Key 和常见凭证，系统不提供完全无脱敏模式。
3. 是否接受 SHA-256 只用于关联，并明确不宣称能够保护低熵内容。
4. 是否接受当前 LLM API Key 作为 known secret 注册到 Observability 内部，并在关闭时清零副本。
5. 是否接受 `LITECRAB_DEBUG_LLM` 在所有模式都只记录 Tool 参数摘要。
6. 是否接受最终日志目录被收紧为 `0700`，现有日志文件被收紧为 `0600`。
7. 是否接受日志目标为 symlink、hardlink、FIFO 或其他非普通文件时服务启动失败。
8. 是否接受 Viewer 在默认模式不再支持业务原文全文搜索。
9. 是否接受本步骤不处理日志轮转和 tamper evidence，将其保留为后续独立能力。

上述结论确认后，本步骤可以独立实施，不依赖 Principal、Policy Engine、审批或 Secret Broker。

