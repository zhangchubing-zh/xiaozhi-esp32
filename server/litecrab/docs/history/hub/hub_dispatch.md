# hub_dispatch：响应分派

## 1. 模块职责

`hub/dispatch` 负责根据消息的**响应模式**把 AgentLoop 产生的出站响应返回给调用方：

- `LITE_REPLY_SYNC`：**阻塞等待** outbound 队列中出现该 requestId 的响应（带超时），返回响应内容。
- `LITE_REPLY_ACK_ONLY`：不等待，直接返回空（只做确认）。

## 2. 核心流程（DispatchResponse）

```
DispatchResponse(requestId, replyMode, timeoutMs, resp, respSize):
    if !resp || respSize==0: return -1

    if replyMode == LITE_REPLY_ACK_ONLY:
        # 仅确认：不等待，清空输出
        resp[0] = '\0'
        return 0

    # --- SYNC 模式 ---
    LiteMsg msg
    rc = MessageBusTimedPopOutboundByRequestId(requestId, &msg, timeoutMs)
    if rc != 0:
        snprintf(resp, respSize, "ERROR: response timeout")
        return -1

    if msg.content:
        snprintf(resp, respSize, "%s", msg.content)
        free(msg.content)              # 释放随消息移转的内容
    else:
        resp[0] = '\0'

    return 0
```

### 2.1 关键行为

- **ACK_ONLY**：调用方（如事件型请求）不需要结果，立即返回空字符串。
- **SYNC**：阻塞在 `TimedPopOutboundByRequestId` 上直到：
  - 找到匹配 requestId 的响应 -> 复制 `content` 返回；
  - 或超过 `timeoutMs` -> 返回 `"ERROR: response timeout"` 并返回 -1。
- 取到响应后**必须释放** `msg.content`（消息总线移转的内存所有权）。

## 3. 与消息总线的配合

Dispatch 不直接持有出站队列，而是委托 message_bus 的按 requestId 定位能力（见 hub_message_bus.md 的 `TimedPopOutboundByRequestId`）。这使得多请求并发时，每个调用方只取到自己请求对应的响应。

## 4. 对外接口签名

```c
int DispatchResponse(const char *requestId, LiteReplyMode replyMode,
                     int timeoutMs, char *resp, int respSize);
```

## 5. 复现要点（检查清单）

- [ ] ACK_ONLY 立即返回且不清空缓冲语义正确（输出设为空串）。
- [ ] SYNC 模式正确阻塞并按 requestId 匹配响应。
- [ ] 超时返回 `"ERROR: response timeout"` 与 -1。
- [ ] 取到响应后释放 msg.content，避免泄漏。

## 6. 相关文档

- `hub_message_bus.md`：TimedPopOutboundByRequestId 实现。
- `gateway_tcp_line.md`：网关在同步模式下调用 DispatchResponse。
