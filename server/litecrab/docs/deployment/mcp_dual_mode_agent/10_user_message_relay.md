# 用户消息、告警与 MCP 的边界

## 1. 核心决策

MCP 用于能力发现和能力调用，不承载普通用户聊天，也不作为高频设备事件总线。文件名保留用于兼容旧链接；本方案不再要求单独的 Agent Link 作为 MCP 落地前置条件。

## 2. 通道划分

| 数据 | Owner | 推荐通道 |
|---|---|---|
| 用户文本/语音意图 | Gateway + Hub + Session | 现有用户协议或专用 WebSocket |
| Agent 回复 | 原用户 Channel | requestId/sessionId 定向回复 |
| ASP 告警 | Alarm spool | 现有持久告警链路 |
| Tool 发现/调用 | 内置 MCP Client Manager | MCP |
| 高频遥测 | Telemetry 服务 | MQTT/专用流 |

## 3. 同一物理连接

xiaozhi 可在一个 WebSocket 上传音频、接收 TTS 并传输 `type=mcp`，但 Gateway 内必须按消息类型分派到独立处理器。音频、用户消息和 Tool 调用分别使用独立队列、并发、超时和指标。

## 4. 身份关联

用户身份、Agent Session 和设备身份是三个概念：

- deviceId 来自设备认证。
- userId 来自可信用户入口。
- sessionId 归 Hub/Session。

设备连接不能通过自报 userId 获得用户权限。Tool 调用授权同时考虑调用用户、Agent policy 和目标 deviceId。

## 5. 回复与 Tool 结果

设备 Tool Result 先返回 Agent Kernel，由模型或确定性工作流决定如何形成用户回复。设备不得把 Tool Result 直接写进用户输出通道。

## 6. 是否需要用户消息中继

只有当现场设备本身也是用户终端、而 Agent 运行在别处时，才增加专用消息中继。该中继使用独立 envelope、ACK、去重和有界 outbox；它不是 MCP Server/Client 的职责，也不改变设备 Tool 架构。

## 7. 当前状态

当前 TCP 用户入口和 Alarm 链路可继续使用。MCP 实施不应先重写这两条链路。
