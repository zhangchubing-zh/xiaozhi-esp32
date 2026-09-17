# gateway_tcp_line：TCP 行协议请求服务器

## 1. 模块职责

`gateway/tcp_line` 提供基于 **TCP 行协议**的外部网络入口：每条请求/响应以 `\n` 结尾，支持**持久连接**（同一连接内多轮请求-响应）。它是 agent 的对外网关，接收客户端请求，转发到 Hub（Ingress），并把响应返回客户端。

## 2. 数据结构

```c
/* 连接上下文：在连接生命周期内传递给 handler */
typedef struct {
    int connFd;              /* 连接 fd */
    char sessionId[64];      /* 内部会话 ID */
    void *user;              /* 用户数据 */
} ConnectionContext;

/* 请求处理回调 */
typedef int (*RequestHandlerFn)(const char *req, char *resp, int respSize, ConnectionContext *ctx);

/* 服务器配置 */
typedef struct {
    char listenIp[64];    /* 监听 IP，如 "0.0.0.0" */
    int listenPort;       /* 监听端口 */
    int backlog;          /* listen backlog */
    int recvTimeoutMs;    /* 接收超时（毫秒） */
    int sendTimeoutMs;    /* 发送超时（毫秒） */
    int maxReqBytes;      /* 单条请求上限（如 16KB） */
} RequestServerConfig;
```

默认参数（main.c 装配时）：端口 **10003**、maxReqBytes **16KB**、backlog **32**、网络超时 **600000ms**（10 分钟）。

## 3. 启动流程（StartRequestServer）

```
StartRequestServer(cfg, handler, user):
    # 1) 创建监听 socket，设置 SO_REUSEADDR
    listenFd = socket(AF_INET, SOCK_STREAM)
    bind(listenFd, listenIp:listenPort)
    listen(listenFd, backlog)

    # 2) accept 主循环（阻塞）
    loop:
        connFd = accept(listenFd, ...)
        # 为每个连接创建独立线程 ClientThread
        create_thread(ClientThread, connFd)
```

**阻塞式**：函数在 accept 循环中运行，直到进程退出。

## 4. 连接线程（ClientThread）—— 核心

```
ClientThread(connFd):
    ConnectionContext ctx
    ctx.connFd = connFd
    ctx.sessionId = GenerateSessionId()       # "sess-%lu-%08x"（时间戳+随机）

    # 1) 配置 socket：设置收发超时、TCP_NODELAY
    setsockopt(recvTimeout, sendTimeout)
    setsockopt(TCP_NODELAY)

    # 2) 会话打开事件
    IngressSubmit(... 构造 SESSION_OPEN 消息（source="network:tcp"）)

    # 3) 请求-响应循环（持久连接）
    loop:
        line = RecvLine(connFd)              # 按 '\n' 读取一行，限长 maxReqBytes
        if line 为空（EOF/超时/超长）: break

        rc = handler(line, resp, RESP_SZ, &ctx)   # 调用注册的处理回调
        if rc < 0: break                          # 失败关闭连接

        确保 resp 以 '\n' 结尾
        SendAll(connFd, resp)

    # 4) 会话关闭事件
    IngressSubmit(... 构造 SESSION_CLOSE 消息)

    close(connFd)
```

### 4.1 GenerateSessionId
```
GenerateSessionId():
    return "sess-" + <当前时间戳> + "-" + <随机数:08x>
```

### 4.2 RecvLine
按行读取，遇 `\n` 返回单行（不含换行）。限长 `maxReqBytes`：超过则视为协议错误关闭。基于 `recv`/缓冲实现，处理半包与粘包。

### 4.3 响应缓冲
响应缓冲大小按 `maxReqBytes * REQ_RESP_BUF_MULTIPLIER`（乘数 4）预留，避免单请求产生超长响应溢出。

### 4.4 已知 P0：多行响应缺少无歧义帧边界

当前响应正文允许包含换行，同时又使用换行作为响应结束标记。TCP 不保留单次 `send` 的消息边界，因此客户端不能可靠区分“正文换行”和“响应结束”；UI 目前通过短暂静默超时收集剩余数据，只是兼容措施。后续 Gateway 整改应统一为长度前缀或单行 JSON envelope，并在协议版本迁移时同步所有客户端，不能继续依赖包边界或静默超时。

## 5. 请求处理回调（main.c 的 OnNetworkRequest）

网关本身不解析业务，真正的解析在回调中：

```
OnNetworkRequest(line, resp, respSize, ctx):
    # 1) 解析 JSON 请求，提取 userId / content
    if TryParseJsonRequest(line) 失败:
        snprintf(resp, RESP_ERR); return 0（或返回错误）

    # 2) 构造 IngressOptions
    opts.source    = "network:tcp"
    opts.userId    = 解析出的 userId（或默认）
    opts.type      = LITE_MSG_CHAT
    opts.priority  = LITE_PRIORITY_NORMAL
    opts.replyMode = LITE_REPLY_SYNC          # 同步等待响应

    # 3) 提交到 Hub，同步取响应
    IngressSubmit("network:tcp", content, len, &opts, &result)
    DispatchResponse(result.requestId, LITE_REPLY_SYNC, timeoutMs, resp, respSize)

    return 0
```

这里完成 **gateway -> hub(ingress) -> kernel(agent loop) -> hub(dispatch) -> gateway** 的完整同步往返。

## 6. 数据流与时序

```
客户端 ──(TCP 行)──► ClientThread
                        │ RecvLine
                        ▼
                   OnNetworkRequest
                        │ IngressSubmit (SYNC)
                        ▼
                   Hub ingress ─PushInbound► AgentLoop
                        │                     │ 处理后 PushOutbound
                        ▼ ◄──────────────┘
                   DispatchResponse (TimedPopOutboundByRequestId)
                        │
                        ▼ SendAll
客户端 ◄──(响应行)──
```

## 7. 对外接口签名

```c
int StartRequestServer(const RequestServerConfig *cfg, RequestHandlerFn handler, void *user);
```

## 8. 复现要点（检查清单）

- [ ] 监听/accept 正确，每连接一个线程。
- [ ] RecvLine 处理半包/粘包，超长请求关闭连接。
- [ ] sessionId 全局唯一（时间戳+随机）。
- [ ] 连接建立/关闭分别发送 SESSION_OPEN / SESSION_CLOSE。
- [ ] handler 返回值 <0 时关闭连接。
- [ ] 响应统一以 `\n` 结尾并 SendAll 完整发送。
- [ ] socket 收发超时生效。

## 9. 相关文档

- `hub_ingress.md`：SESSION_OPEN/CHAT/SESSION_CLOSE 消息提交。
- `hub_dispatch.md`：SYNC 响应分派。
