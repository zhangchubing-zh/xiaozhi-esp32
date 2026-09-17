# hub_message_bus：环形队列消息总线与共享配置

## 1. 模块职责

Hub 层提供进程内**线程间消息传递**机制，作为网关到内核、内核到分发之间的通道。消息总线分为两个方向：

- **inbound（入站）**：Ingress 把请求推入 inbound 队列，AgentLoop 从 inbound 消费处理。
- **outbound（出站）**：AgentLoop 把响应推入 outbound 队列，Dispatch 从 outbound 取出返回给调用方。

本模块同时定义 Hub 层共享配置与类型（`lite_config.h`）。

## 2. 共享配置与类型（lite_config.h）

```c
#define LITE_BUS_QUEUE_LEN 16       /* 单向队列容量 */
#define LITE_OUTBOUND_WORKERS 1     /* 出站分发工作线程数 */
#define LITE_AGENT_MAX_TOOL_ITER 4  /* AgentLoop 单轮最大工具迭代 */

#define LITE_MAX_CHANNEL_LEN 32
#define LITE_MAX_CHAT_ID_LEN 64
#define LITE_MAX_REQUEST_ID_LEN 64
#define LITE_MAX_SOURCE_LEN 32
#define LITE_MAX_USER_ID_LEN 64
#define LITE_MAX_SESSION_ID_LEN 64
```

```c
typedef enum {
    LITE_MSG_CHAT,            /* 聊天 */
    LITE_MSG_EXCEPTION,       /* 异常 */
    LITE_MSG_STATUS_QUERY,    /* 状态查询 */
    LITE_MSG_ACTION_REQUEST,  /* 动作请求 */
    LITE_MSG_SESSION_OPEN,    /* 会话打开 */
    LITE_MSG_SESSION_CLOSE    /* 会话关闭 */
} LiteMsgType;

typedef enum { LITE_PRIORITY_LOW=0, NORMAL=1, HIGH=2, CRITICAL=3 } LitePriority;
typedef enum { LITE_REPLY_SYNC, LITE_REPLY_ACK_ONLY } LiteReplyMode;

/* Ingress 状态码 */
#define INGRESS_ACCEPTED 0
#define INGRESS_REJECTED 1
#define INGRESS_BUSY 2
```

### LiteMsg 消息体

```c
typedef struct {
    LiteMsgType type;
    LitePriority priority;
    LiteReplyMode replyMode;
    char source[32];      /* 请求来源，如 "network:tcp" */
    char channel[32];     /* 频道 */
    char chatId[64];
    char userId[64];
    char sessionId[64];
    char requestId[64];   /* 请求 ID（用于定位出站响应） */
    char *content;        /* 消息内容（堆分配，需释放） */
} LiteMsg;
```

**内存约定**：`content` 为堆分配字符串，弹出消息的消费者负责释放。

## 3. 消息总线实现（message_bus）

### 3.1 环形队列结构

```c
typedef struct {
    LiteMsg q[LITE_BUS_QUEUE_LEN];   /* 16 */
    int head, tail, count;
    pthread_mutex_t mutex;
    pthread_cond_t notEmpty, notFull;
} BusQueue;

/* 两个静态队列 */
static BusQueue g_inbound;
static BusQueue g_outbound;
```

每个队列用**互斥锁 + 两个条件变量**实现线程安全的有界缓冲区。

### 3.2 初始化

```
MessageBusInit():
    初始化 g_inbound 与 g_outbound 的 mutex / notEmpty / notFull
    head=tail=count=0
```

### 3.3 推入（Push）

```
Push(queue, msg):
    lock(mutex)
    while queue.count == LITE_BUS_QUEUE_LEN:   # 队满阻塞
        cond_wait(queue.notFull, mutex)
    queue.q[queue.tail] = *msg                  # 拷贝消息体（content 指针随入队移转）
    queue.tail = (queue.tail+1) % LITE_BUS_QUEUE_LEN
    queue.count++
    cond_signal(queue.notEmpty)
    unlock(mutex)
```

### 3.4 弹出（Pop）

```
Pop(queue, out):
    lock(mutex)
    while queue.count == 0:                     # 队空阻塞
        cond_wait(queue.notEmpty, mutex)
    *out = queue.q[queue.head]
    queue.head = (queue.head+1) % LITE_BUS_QUEUE_LEN
    queue.count--
    cond_signal(queue.notFull)
    unlock(mutex)
```

### 3.5 按请求 ID 弹出（PopOutboundByRequestId）

用于同步响应对应定位：

```
FindRequestPos(queue, requestId):
    # 从 head 开始线性扫描 count 个元素，比较 requestId
    # 返回匹配下标，未找到返回 -1

PopOutboundByRequestId(requestId, out):
    lock(mutex)
    pos = FindRequestPos(g_outbound, requestId)
    if pos < 0: unlock; return -1
    记录 matched
    # 若匹配元素在 head，直接取出 head（O(1)）
    # 否则用 RemoveAtLocked 把该元素取出并将后续元素前移
    RemoveAtLocked(g_outbound, pos, out)
    unlock; return 0
```

### 3.6 带超时按请求 ID 弹出（TimedPopOutboundByRequestId）

```
TimedPopOutboundByRequestId(requestId, out, timeoutMs):
    lock(mutex)
    start = now_ms()
    loop:
        pos = FindRequestPos(g_outbound, requestId)
        if pos >= 0:
            RemoveAtLocked(...); unlock; return 0
        # 未找到：等待 notEmpty，但可能到达者不是本 requestId
        remaining = timeoutMs - elapsed
        if remaining <= 0: resetTimeout; unlock; return -1
        cond_timedwait(queue.notEmpty, mutex, remaining)
```

这是 Dispatch 同步等待响应的核心：**按 requestId 精确匹配**，而非先进先出。

## 4. 线程模型与数据流

```
                      ┌──────────────┐
  Ingress ──Push──►   g_inbound      ◄──Pop── AgentLoop
                      └──────────────┘
                                    │ (处理后)
                                    ▼
                      ┌──────────────┐
  Dispatch ◄──Pop──   g_outbound     ◄──Push── AgentLoop(resp)
                      └──────────────┘
```

- Ingress、AgentLoop、Dispatch 运行在不同线程。
- 队列有界（16），队满 Push 阻塞（背压），队空 Pop 阻塞（等待）。
- outbound 通过 requestId 定位，支持多请求并发时正确返回各自响应。

## 5. 对外接口签名

```c
int MessageBusInit(void);
int MessageBusPushInbound(const LiteMsg *msg);
int MessageBusPopInbound(LiteMsg *msg);
int MessageBusPushOutbound(const LiteMsg *msg);
int MessageBusPopOutbound(LiteMsg *msg);
int MessageBusPopOutboundByRequestId(const char *requestId, LiteMsg *msg);
int MessageBusTimedPopOutboundByRequestId(const char *requestId, LiteMsg *msg, int timeoutMs);
```

## 6. 复现要点（检查清单）

- [ ] 环形队列容量 16，head/tail 模运算正确。
- [ ] Push 队满阻塞、Pop 队空阻塞，条件变量与互斥锁配对正确。
- [ ] `content` 所有权随消息移转，消费者负责 free。
- [ ] 按 requestId 弹出支持从队列中部取出并前移。
- [ ] 带超时弹出在超时后返回 -1，不泄漏锁。

## 7. 相关文档

- `hub_ingress.md`：入站消息如何构造 LiteMsg 并 PushInbound。
- `hub_dispatch.md`：出站响应如何按 requestId 取出。
