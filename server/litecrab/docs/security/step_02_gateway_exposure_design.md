# 第 2 步实施设计：收紧 Gateway 默认暴露范围

状态：设计完成，待评审确认后实施  
对应计划：[安全能力渐进式实施计划](security_incremental_implementation_plan.md#43-第-2-步收紧-gateway-默认暴露范围)  
设计基线：2026-09-02

## 1. 本步骤目标

在 LiteCrab 尚未具备可信客户端认证的阶段，将 TCP Gateway 的默认网络暴露范围限制在本机，并让任何未认证的远程监听都必须经过清晰、显式、可追踪的风险确认。

本步骤完成后应满足：

1. 未提供 Base Config 时，Gateway 默认绑定 `127.0.0.1`。
2. Base Config 未指定 `listen_ip` 时，Gateway 默认绑定 `127.0.0.1`。
3. `listen_ip` 为非 loopback IPv4 地址时，默认拒绝启动。
4. 只有配置文件显式设置 `allow_unauthenticated_remote: true`，才允许绑定非 loopback 地址。
5. `StartRequestServer` 自身再次执行绑定策略校验，不能依赖调用方已经通过配置加载器。
6. 启动日志明确说明 TCP 未认证以及请求体中的 `userId` 不可信。
7. 默认本机 TCP 调用方式和现有 UI、测试流程继续工作。

## 2. 安全边界说明

### 2.1 本步骤能够解决的问题

- 避免默认 `0.0.0.0` 将 Agent 暴露给局域网或设备其他网络接口。
- 避免操作者只修改 `listen_ip` 就无意中开放未认证服务。
- 避免直接调用 `StartRequestServer` 绕过配置层的远程监听限制。
- 让高风险兼容模式在配置和日志中具有明确语义。

### 2.2 本步骤不能解决的问题

- Loopback 不是身份认证；同一设备上的其他进程仍可能连接 TCP 端口。
- 请求 JSON 中的 `userId` 仍由客户端自报，不能作为安全 Principal。
- TCP 内容仍是明文，没有消息完整性、来源认证和防重放。
- 本步骤不实现 Unix Domain Socket、`SO_PEERCRED`、HMAC、TLS 客户端证书或设备 IAM。
- 本步骤不实现连接数、速率和慢连接限制，这属于后续 Gateway 资源预算步骤。
- 本步骤不改变 Session owner 的现有 `userId` 绑定逻辑；可信 Principal 将在第 6 步引入。

因此，`allow_unauthenticated_remote: true` 只表示操作者明确接受未认证远程 TCP 的风险，不代表该连接已经安全或已认证。

## 3. 当前代码状态

### 3.1 默认值存在两个来源

当前监听默认值分别存在于：

1. `LiteCrabConfigDefaults`：将 `server.listenIp` 设为 `0.0.0.0`。
2. `StartRequestServer`：当调用方传入空地址时，再次回退到 `0.0.0.0`。

只修改其中一个位置不能形成可靠默认值：其他代码可以绕过 `LiteCrabConfigLoadProvider`，直接构造 `RequestServerConfig` 并调用 `StartRequestServer`。

### 3.2 当前配置加载链

```text
LiteCrabConfigDefaults
    |
load_base(JSON)
    |
load_llm(JSON)
    |
LITECRAB_* 环境变量覆盖
    |
validate
    |
main 中 --workspace / --port 覆盖
    |
StartRequestServer
```

当前 `load_base` 对 Server 配置使用宽松读取：字段不存在或类型错误时，大部分情况下继续保留默认值。`listen_ip` 也没有在配置层验证为合法 IPv4 地址，最终由 `StartRequestServer` 中的 `inet_pton` 间接拒绝。

### 3.3 当前监听行为

`StartRequestServer` 使用 `AF_INET` 和 `sockaddr_in`，只支持数字形式的 IPv4 地址，不支持：

- IPv6；
- 主机名；
- `localhost` 字符串；
- Unix Domain Socket。

因此本步骤的 loopback 判定只需要覆盖 IPv4 `127.0.0.0/8`，不扩展地址族和解析能力。

### 3.4 当前启动顺序和日志

`main.c` 当前在调用阻塞式 `StartRequestServer` 前记录：

```text
[gateway] LiteCrab listening on <ip>:<port>
```

这条日志没有说明：

- TCP 是否认证；
- `userId` 是否可信；
- 非 loopback 是否是显式风险模式；
- 此时实际 `bind/listen` 尚未成功。

### 3.5 现有测试依赖

当前所有 Gateway 相关测试都通过 `127.0.0.1` 连接：

- `tests/test_tcp_e2e.py`
- `tests/test_agent_matrix.py`
- `tests/test_session_restart.py`
- `tests/test_security_e2e.py`
- `tests/test_ui_session_transport.py`

默认绑定改为 `127.0.0.1` 不应破坏这些测试，但必须新增负向测试，证明非 loopback 默认确实被拒绝，而不是仅验证 loopback 仍可连接。

## 4. 配置契约

### 4.1 新增配置项

在 `server` 对象中增加：

```json
{
  "server": {
    "listen_ip": "127.0.0.1",
    "allow_unauthenticated_remote": false
  }
}
```

字段语义：

| 字段 | 类型 | 默认值 | 含义 |
|---|---|---:|---|
| `listen_ip` | string | `127.0.0.1` | 监听的数字 IPv4 地址 |
| `allow_unauthenticated_remote` | boolean | `false` | 是否显式允许未认证 TCP 绑定非 loopback 地址 |

### 4.2 配置优先级

本步骤保持现有总体优先级：默认值 < JSON 配置 < 已有环境变量 < 已有命令行参数。

但 `allow_unauthenticated_remote` 是高风险确认项，本步骤不增加以下覆盖方式：

- 不增加 `LITECRAB_ALLOW_UNAUTHENTICATED_REMOTE` 环境变量；
- 不增加 `--allow-unauthenticated-remote` 命令行参数；
- 不允许请求体或 Workspace Skill 修改该值。

操作者必须在 Base Config 中留下明确、可审查的配置记录。

### 4.3 严格字段类型

`allow_unauthenticated_remote` 存在时必须是 JSON boolean。以下值都应导致配置加载失败：

```json
"allow_unauthenticated_remote": "true"
"allow_unauthenticated_remote": 1
"allow_unauthenticated_remote": null
```

本步骤只对新增安全字段执行严格类型校验，不提前实现第 8 步的完整 Security Config 未知字段拒绝。

### 4.4 地址判定规则

地址必须先通过 `inet_pton(AF_INET, ...)`。

将地址转换为 host byte order 后，满足下式视为 loopback：

```c
(address & 0xff000000U) == 0x7f000000U
```

即接受整个 IPv4 loopback 网段 `127.0.0.0/8`，包括 `127.0.0.1` 和 `127.0.0.2`。

判定矩阵：

| `listen_ip` | 兼容开关 | 结果 |
|---|---:|---|
| `127.0.0.1` | false | 允许 |
| `127.0.0.2` | false | 允许 |
| `0.0.0.0` | false | 拒绝 |
| 局域网地址 | false | 拒绝 |
| 公网地址 | false | 拒绝 |
| `0.0.0.0` | true | 允许并记录高风险警告 |
| 局域网地址 | true | 允许并记录高风险警告 |
| 非法 IPv4 字符串 | 任意 | 拒绝 |
| `localhost` | 任意 | 拒绝，保持当前只接受数字 IPv4 的契约 |

## 5. API 与数据结构修改

### 5.1 `RequestServerConfig`

文件：`include/litecrab/gateway.h`

增加字段：

```c
int allowUnauthenticatedRemote;
```

建议放在 `listenIp` 之后或现有整数配置末尾，并在注释中明确只允许 0/1。

### 5.2 统一绑定策略校验函数

文件：

- `include/litecrab/gateway.h`
- `src/gateway/tcp_line.c`

新增：

```c
int RequestServerValidateBindPolicy(const RequestServerConfig* config,
                                    char* error,
                                    size_t errorSize);
```

为使用 `size_t`，`gateway.h` 需要显式包含 `<stddef.h>`。

函数职责限定为：

1. 检查 `listenIp` 是否是合法数字 IPv4。
2. 检查 `allowUnauthenticatedRemote` 是否为 0 或 1。
3. 判断地址是否属于 `127.0.0.0/8`。
4. 非 loopback 且开关为 0 时拒绝。
5. 返回不包含敏感数据的稳定错误文本。

建议错误文本：

```text
invalid IPv4 listen address: <address>
invalid allowUnauthenticatedRemote value
unauthenticated remote TCP requires allow_unauthenticated_remote=true
```

该函数不负责创建 socket、绑定端口或记录日志，便于配置层和测试直接复用。

## 6. 两层强制校验

### 6.1 第一层：配置加载阶段

`src/config/config.c` 中的 `validate` 调用 `RequestServerValidateBindPolicy`。

效果：主程序在初始化日志、Agent Loop、Session 和 LLM 之前就能对不安全配置 fail-closed，并通过现有配置错误通道退出：

```text
configuration error: unauthenticated remote TCP requires ...
```

### 6.2 第二层：Gateway 启动阶段

`StartRequestServer` 复制传入配置并补齐默认值后，再调用 `RequestServerValidateBindPolicy`，且必须发生在 `socket()` 之前。

效果：

- 直接调用 Gateway API 也不能绕过绑定策略；
- 非法配置不会创建监听 socket；
- 未来出现新的启动入口时仍有最终防线。

`StartRequestServer` 当前只返回 `-1`，不新增复杂错误枚举。详细错误由配置层或启动日志提供；后续统一 Gateway Error 契约可单独设计。

## 7. 文件级修改说明

### 7.1 `include/litecrab/gateway.h`

修改内容：

1. 包含 `<stddef.h>`。
2. 为 `RequestServerConfig` 增加 `allowUnauthenticatedRemote`。
3. 声明 `RequestServerValidateBindPolicy`。

不修改：

- `ConnectionContext`；
- `RequestHandlerFn`；
- `StartRequestServer` 的函数签名；
- TCP 请求协议。

### 7.2 `include/litecrab/config.h`

根据当前结构，本步骤不需要直接修改该文件。

`LiteCrabAppConfig` 已经内嵌 `RequestServerConfig server`，新增字段会通过 `gateway.h` 自动进入 App Config。原简要计划将其列为预计修改文件，实际实施应避免无意义改动。

### 7.3 `src/config/config.c`

修改内容一：安全默认值。

在 `LiteCrabConfigDefaults` 中：

```text
listenIp                     = "127.0.0.1"
allowUnauthenticatedRemote   = 0
```

修改内容二：解析新增字段。

在 `load_base` 的 `server` 分支中读取 `allow_unauthenticated_remote`。字段存在但 `get_bool` 失败时，必须设置配置错误并返回失败，不能静默保留默认值。

修改内容三：统一校验。

在 `validate` 中调用 `RequestServerValidateBindPolicy`。调用顺序建议在端口和 Server limits 校验之前，使监听暴露错误尽早返回。

不增加环境变量和命令行覆盖。

### 7.4 `src/gateway/tcp_line.c`

修改内容一：实现 `RequestServerValidateBindPolicy`。

内部使用 `inet_pton`、`ntohl` 和 `INADDR_LOOPBACK` 对应的 `/8` 掩码完成判定，不进行 DNS 解析。

修改内容二：修改 Gateway 自身默认值。

`StartRequestServer` 收到空 `listenIp` 时，回退到 `127.0.0.1`，不再回退到 `0.0.0.0`。

修改内容三：最终强制校验。

在 `socket()` 之前调用绑定策略校验。失败时立即返回 `-1`，确保没有 socket 副作用。

不修改：

- `accept` 和每连接线程模型；
- 请求读取、超长请求和响应格式；
- `userId` 解析逻辑；
- Session ID 生成逻辑。

### 7.5 `src/main.c`

在 `InitLogWithDir` 成功后、启动 Agent Loop 前记录安全状态。

Loopback 模式建议记录：

```text
[security] TCP gateway has no client authentication; request userId is untrusted; exposure=loopback bind=127.0.0.1:<port>
```

显式远程兼容模式建议记录：

```text
[security] WARNING unauthenticated remote TCP is explicitly enabled; request userId is untrusted; bind=<ip>:<port>
```

日志只包含监听地址、端口和安全模式，不记录：

- API Key；
- 请求内容；
- Session 内容；
- 客户端提供的 `userId`。

现有 `[gateway] LiteCrab listening ...` 建议改为 `[gateway] LiteCrab starting ...`，避免在 `bind/listen` 实际成功前声称已经监听。

### 7.6 `config/base_config.example.json`

修改为安全默认示例：

```json
{
  "server": {
    "listen_ip": "127.0.0.1",
    "allow_unauthenticated_remote": false,
    "listen_port": 10003
  }
}
```

示例中不得把兼容开关设为 true，否则用户复制示例后会重新恢复不安全暴露。

### 7.7 `README.md`

增加以下说明：

- 默认只监听 `127.0.0.1`。
- Loopback TCP 仍没有客户端认证。
- JSON 中的 `userId` 当前只用于会话兼容，不是可信身份。
- 非 loopback 监听需要显式设置 `allow_unauthenticated_remote: true`。
- 该开关是高风险兼容模式，生产跨主机访问应经过已有认证网关或等待后续认证能力。
- PC Transfer Station 如果需要从另一台机器连接板端 LiteCrab，必须明确配置这一兼容模式并承担风险。

### 7.8 `tests/test_security.c`

包含 `litecrab/gateway.h`，新增绑定策略表驱动测试：

| 地址 | 开关 | 预期 |
|---|---:|---|
| `127.0.0.1` | 0 | 通过 |
| `127.0.0.2` | 0 | 通过 |
| `0.0.0.0` | 0 | 拒绝 |
| `192.168.1.20` | 0 | 拒绝 |
| `0.0.0.0` | 1 | 通过 |
| `192.168.1.20` | 1 | 通过 |
| `not-an-ip` | 0/1 | 拒绝 |
| `localhost` | 0/1 | 拒绝 |
| `127.0.0.1` | 2 | 拒绝 |

测试同时断言错误缓冲区不为空，但不依赖完整自然语言文本，避免无意义的文案耦合。

### 7.9 `tests/test_main.c`

配置测试需要补充：

1. `LiteCrabConfigDefaults` 默认地址是 `127.0.0.1`，开关为 0。
2. Base Config 使用 `0.0.0.0` 且未设置开关时加载失败。
3. Base Config 使用 `0.0.0.0` 且显式设置 true 时加载成功。
4. 新增字段类型不是 boolean 时加载失败。
5. 非法 IPv4 字符串加载失败。

现有显式 `127.0.0.1` 配置测试继续保留。

### 7.10 `tests/test_security_e2e.py`

新增进程级启动策略测试：

1. 写入临时 Base Config：`listen_ip=0.0.0.0`，不允许远程。
2. 启动 `litecrab_server --config <file>`。
3. 断言进程以配置错误退出，退出码为 2。
4. 断言 stderr 包含未认证远程 TCP 被拒绝的稳定关键词。
5. 断言目标端口没有监听。

再增加显式兼容场景：

1. 写入 `listen_ip=0.0.0.0` 且 `allow_unauthenticated_remote=true`。
2. 启动服务并通过 `127.0.0.1` 连接。
3. 使用不触发 LLM 的非法 Session 请求确认 Gateway 工作。
4. 检查日志包含高风险警告且不包含测试 API Key。

### 7.11 `tests/test_tcp_e2e.py`

现有测试不传 Base Config，因而天然覆盖“默认配置下 loopback 客户端仍可使用完整 Agent 流程”。预计不需要修改测试逻辑，只需作为本步骤相关回归执行。

如果评审要求 E2E 明确检查默认配置值，应优先放在 `test_security_e2e.py`，不应让正常 TCP 功能测试依赖 `/proc` 或系统命令来判断监听 socket 地址。

### 7.12 活跃文档同步

功能验收后更新：

- `docs/security/security_incremental_implementation_plan.md`：标记第 2 步完成并记录测试结果。
- `docs/security/security_architecture_design.md`：将“默认 0.0.0.0”改为已经完成的 loopback 默认控制，同时保留“无认证”缺口。
- `docs/gateway/5091_ipc_gateway_design.md`：更新其中仍描述当前默认值为 `0.0.0.0` 的现状段落。

`docs/history/` 下的历史文档不修改。

## 8. 修改文件汇总

| 文件 | 类型 | 修改目的 |
|---|---|---|
| `include/litecrab/gateway.h` | 生产代码 | 增加远程兼容字段和统一绑定策略校验 API |
| `src/config/config.c` | 生产代码 | 安全默认值、严格解析新增字段、配置阶段校验 |
| `src/gateway/tcp_line.c` | 生产代码 | loopback fallback 和 socket 创建前最终校验 |
| `src/main.c` | 生产代码 | 记录未认证状态和远程高风险模式 |
| `config/base_config.example.json` | 配置示例 | 提供安全默认配置 |
| `README.md` | 使用文档 | 说明默认暴露范围、风险开关及身份限制 |
| `tests/test_security.c` | 安全单元测试 | 表驱动验证地址和风险开关矩阵 |
| `tests/test_main.c` | 配置单元测试 | 验证默认值、严格类型和 fail-closed |
| `tests/test_security_e2e.py` | 安全 E2E | 验证不安全配置拒绝和显式兼容启动 |
| `tests/test_tcp_e2e.py` | 相关回归 | 不一定修改，但必须执行 |
| `docs/security/security_incremental_implementation_plan.md` | 计划文档 | 完成后记录实施状态 |
| `docs/security/security_architecture_design.md` | 活跃设计文档 | 同步已经落地的控制和剩余缺口 |
| `docs/gateway/5091_ipc_gateway_design.md` | 活跃设计文档 | 修正当前 Gateway 默认值描述 |

预计不修改：

- `include/litecrab/config.h`：App Config 已内嵌 `RequestServerConfig`。
- `src/kernel/kernel.c`：本步骤不改变 Agent 执行链。
- `src/session/session.c`：本步骤不改变现有 owner 绑定。
- `others/UI/app.py`：当前 UI 已连接 `127.0.0.1`，无需协议调整。

## 9. 修改后的数据流总结

### 9.1 默认启动

```text
无 Base Config / 未设置 listen_ip
    |
LiteCrabConfigDefaults
    | listenIp = 127.0.0.1
    | allowUnauthenticatedRemote = 0
    v
RequestServerValidateBindPolicy
    | 合法 IPv4：是
    | loopback：是
    v
main 记录“未认证、仅 loopback”
    |
StartRequestServer 再次校验
    |
socket -> bind(127.0.0.1) -> listen
```

### 9.2 非 loopback 默认拒绝

```text
Base Config: listen_ip = 0.0.0.0
             allow_unauthenticated_remote = false/缺省
    |
load_base
    |
validate -> RequestServerValidateBindPolicy
    |
    +--> 拒绝：unauthenticated remote TCP requires explicit opt-in
             |
             +-- 不初始化 Agent Loop
             +-- 不创建监听 socket
             +-- main 返回配置错误码 2
```

### 9.3 显式远程兼容模式

```text
Base Config: listen_ip = 0.0.0.0
             allow_unauthenticated_remote = true
    |
严格 boolean 解析
    |
配置校验允许
    |
main 记录 WARNING + bind 地址 + userId 不可信
    |
StartRequestServer 再次校验允许
    |
bind/listen
```

### 9.4 直接调用 Gateway API

```text
其他调用方手工构造 RequestServerConfig
    |
绕过 LiteCrabConfigLoadProvider
    |
StartRequestServer 补齐空地址为 127.0.0.1
    |
RequestServerValidateBindPolicy
    |
允许安全配置 / 拒绝未授权远程配置
    |
只有通过后才创建 socket
```

## 10. 测试矩阵

| 层级 | 场景 | 预期结果 | 副作用检查 |
|---|---|---|---|
| Config | 无 Base Config | `127.0.0.1`、开关为 0 | 无远程监听 |
| Config | loopback + false | 加载成功 | 无 |
| Config | `0.0.0.0` + false | 配置失败 | 未启动 Agent/Socket |
| Config | LAN IP + false | 配置失败 | 未启动 Agent/Socket |
| Config | `0.0.0.0` + true | 加载成功 | 后续记录警告 |
| Config | 开关为字符串或整数 | 配置失败 | 不采用宽松真值 |
| Config | 非法 IPv4 | 配置失败 | 不进入 bind |
| Gateway API | 空 listenIp | 回退 `127.0.0.1` | 不回退到 wildcard |
| Gateway API | remote + false | `StartRequestServer` 返回失败 | socket 创建前拒绝 |
| E2E | 默认配置 | loopback 客户端正常工作 | 原有 Agent 流程通过 |
| E2E | remote + false | 进程退出码 2 | 端口未监听 |
| E2E | remote + true | 服务启动 | 日志有 WARNING |
| Log | loopback | 记录未认证和 userId 不可信 | 不含 API Key |
| Log | remote compatibility | 记录高风险 WARNING | 不含请求和凭证 |

## 11. 兼容性影响

### 11.1 明确的行为变化

- 不带配置启动时，不再接受其他设备通过非 loopback 接口连接。
- 原有 `listen_ip: 0.0.0.0` 配置如果没有新增确认项，将启动失败。
- 非法 IPv4 地址会在配置阶段失败，而不是延迟到 Gateway bind 阶段。
- 启动日志会明确显示当前未认证状态。

### 11.2 保持兼容的部分

- `127.0.0.1:<port>` 协议、请求格式和响应格式不变。
- UI 默认连接方式不变。
- `--port` 和旧位置参数仍兼容。
- Session ID 和 `userId` 字段格式不变。
- 显式远程兼容模式可以暂时保留原有跨设备 TCP 部署。

### 11.3 部署注意事项

PC Transfer Station 或其他跨主机客户端如果直接连接板端 LiteCrab，需要在本步骤实施前确认部署拓扑：

- 同设备部署：保持默认 `127.0.0.1`。
- 跨设备直连：临时显式设置 `allow_unauthenticated_remote: true`，并视为高风险兼容模式。
- 生产跨设备部署：优先通过设备已有认证代理访问，不应把兼容开关当成最终安全方案。

## 12. 实施顺序

建议顺序：

1. 在 `gateway.h` 增加字段和校验函数声明。
2. 在 `tcp_line.c` 实现地址解析与绑定策略表驱动测试所需逻辑。
3. 在 `tests/test_security.c` 增加地址/开关矩阵，先验证校验函数。
4. 在 `config.c` 修改默认值并严格解析新增字段。
5. 在 `tests/test_main.c` 增加配置默认值和失败场景。
6. 在 `StartRequestServer` 增加 socket 创建前的第二次校验。
7. 在 `main.c` 增加安全状态日志。
8. 更新示例配置和 README。
9. 在 `test_security_e2e.py` 增加进程启动拒绝及显式兼容场景。
10. 运行安全测试、TCP E2E 和全量回归。
11. 验收通过后同步活跃架构文档和总计划状态。

## 13. 验证命令与通过标准

使用全新或重新配置的构建目录：

```sh
cmake -S . -B build-step2 -DLITECRAB_ENABLE_STATIC_LINK=OFF
cmake --build build-step2 -j2
ctest --test-dir build-step2 -R litecrab_security --output-on-failure
ctest --test-dir build-step2 -R litecrab_tcp_e2e --output-on-failure
ctest --test-dir build-step2 --output-on-failure
```

通过标准：

1. 缺省配置值是 `127.0.0.1` 和 `allowUnauthenticatedRemote == 0`。
2. 未授权的 `0.0.0.0` 或 LAN 地址在配置阶段 fail-closed。
3. 直接调用 `StartRequestServer` 也无法绕过绑定策略。
4. 显式 `allow_unauthenticated_remote: true` 可以保留兼容启动路径。
5. 风险开关只接受 JSON boolean。
6. 默认 TCP Agent E2E 正常通过。
7. 安全日志说明 TCP 未认证和 `userId` 不可信。
8. 日志不包含测试 API Key、请求内容和其他凭证。
9. 全量测试通过。
10. `git diff --check` 无错误。

## 14. 回退边界

本步骤不修改持久化文件格式和 TCP 消息协议。代码、配置示例、测试与文档应形成一个独立提交，可以整体回退。

不能只回退其中一层：

- 只保留配置层校验会让直接 Gateway 调用者绕过限制。
- 只保留 Gateway 层校验会让主程序在初始化 Agent 后才发现错误，错误信息也更弱。
- 只改默认地址、不增加显式远程确认，会让旧的不安全 Base Config 继续静默开放。
- 只增加警告、不拒绝配置，不能构成安全控制。

## 15. 评审重点

实施前建议确认：

1. 是否接受 `127.0.0.0/8` 全部视为 loopback，而不是只接受精确字符串 `127.0.0.1`。
2. 是否接受高风险兼容开关名称 `allow_unauthenticated_remote`。
3. 是否接受该开关只能来自 Base Config，不提供环境变量和命令行覆盖。
4. 是否接受 `0.0.0.0` 等旧配置在没有新开关时直接启动失败。
5. 是否接受启动日志明确说明当前 `userId` 不可信。
6. 是否接受本步骤不改变 Session owner 逻辑，将可信 Principal 留到第 6 步。
7. 是否需要为现有跨设备 Transfer Station 部署预先准备一份显式高风险兼容配置示例；默认示例仍必须保持 loopback 和 false。

上述结论确认后，本步骤无需依赖 Policy Engine、审批或身份模块即可独立实施和验证。

