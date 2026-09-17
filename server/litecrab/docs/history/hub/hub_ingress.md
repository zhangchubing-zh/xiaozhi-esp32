# hub_ingress：请求入口处理

## 1. 模块职责

`hub/ingress` 负责接收**原始请求**，校验并组装为 `LiteMsg`，推入 inbound 消息总线。它是**外部请求进入消息总线的唯一入口**（包括网关、IPC 等所有来源）。

```
原始请求(rawReq, 长度) + 选项(source/userId/sessionId/type/priority/replyMode)
        │
        ▼
   IngressSubmit
        │  校验、应用默认值、派生 channel、生成 requestId
        ▼
   PushInbound(LiteMsg)
```

## 2. 数据结构

```c
typedef struct {
    const char *source;    /* 请求来源标识 */
    const char *userId;    /* 用户（可选） */
    const char *sessionId; /* 会话（可选） */
    LiteMsgType type;
    LitePriority priority;
    LiteReplyMode replyMode;
} IngressOptions;

typedef struct {
    int status;                    /* ACCEPTED/REJECTED/BUSY */
    char requestId[64];
} IngressResult;
```

## 3. 核心流程（IngressSubmit）

```
IngressSubmit(source, rawReq, rawReqLen, options, result):
    # 1) 长度校验
    if !rawReq || rawReqLen <= 0:
        result.status = REJECTED; return -1
    if rawReqLen > INGRESS_MAX_REQ_BYTES (16KB):
        result.status = REJECTED; return -1          # 请求超限

    # 2) 构造 LiteMsg（memset 清零）
    msg.type      = options ? options.type : LITE_MSG_CHAT
    msg.priority  = options ? options.priority : LITE_PRIORITY_NORMAL
    msg.replyMode = options ? options.replyMode : LITE_REPLY_ACK_ONLY

    # 3) 来源派生 channel
    #    channel = source 中 ':' 前的部分
    #    例：source="network:tcp" -> channel="network"
    DeriveChannel(source, msg.channel)

    # 4) 应用默认值
    if user/sessionId 未指定: userId="anonymous", sessionId="default"
    if chatId 未指定: chatId 沿用 sessionId（或 "default"）

    msg.source   = source
    msg.userId   = userId
    msg.sessionId = sessionId

    # 5) 生成唯一 requestId
    lock(g_counter_mutex)
    msg.requestId = "req-%lu" % (++g_requestSeq)
    unlock

    # 6) 深拷贝 content（strdup），由消费者释放
    msg.content = strdup(rawReq)

    # 7) 推入入站队列
    if MessageBusPushInbound(&msg) != 0:
        free(msg.content)
        result.status = BUSY; return -1
    result.status = ACCEPTED
    result.requestId = msg.requestId
    return 0
```

### 3.1 常量

- `INGRESS_MAX_REQ_BYTES = 16KB`：单条请求最大长度，超限拒绝。
- requestId 格式：`req-<自增序号>`，跨线程用互斥锁保证唯一。

### 3.2 DeriveChannel

`source` 形如 `"network:tcp"`；`channel` 取 `:` 之前的部分（即来源类型名）。用于后续路由或日志归组。

## 4. 调用方示例

网关的 `OnNetworkRequest` 会构造类似：

```
opts.source = "network:tcp"
opts.userId = 解析出的 userId
opts.type   = LITE_MSG_CHAT
opts.priority = LITE_PRIORITY_NORMAL
opts.replyMode = LITE_REPLY_SYNC
IngressSubmit("network:tcp", jsonStr, len, &opts, &result)
```

## 5. 对外接口签名

```c
int IngressInit(void);
int IngressSubmit(const char *source, const char *rawReq, int rawReqLen,
                  const IngressOptions *options, IngressResult *result);
```

## 6. 复现要点（检查清单）

- [ ] 超长请求（>16KB）被拒。
- [ ] 未指定 userId/sessionId 时填充正确默认值（anonymous/default）。
- [ ] requestId 全局唯一，格式 `req-N`。
- [ ] content 深拷贝，消费者负责释放；Push 失败时不泄漏。
- [ ] channel 从 source 的 `:` 前缀正确派生。
- [ ] 队列满时返回 BUSY。

## 7. 相关文档

- `hub_message_bus.md`：inbound 队列与 LiteMsg。
- `gateway_tcp_line.md`：网关如何调用 IngressSubmit。
