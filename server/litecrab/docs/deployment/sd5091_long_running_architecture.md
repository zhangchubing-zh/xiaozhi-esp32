# LiteCrab 在 SD5091 上长期运行的可靠性架构设计

状态：现状评估与目标方案  
评估基线：2026-09-10  
适用范围：SD5091，32 位 ARM 用户空间，LiteCrab 常驻 Agent 服务  
相关文档：[`sd5091_deployment.md`](sd5091_deployment.md)、[`当前模块文档入口`](../current_architecture_index.md)、[`当前安全架构`](../security/security_module_architecture.md)

## 1. 结论

LiteCrab 当前版本可以在 SD5091 上启动并完成基本 Agent、Tool、Session 和告警流程，现有 WSL 测试也能通过；但当前架构不适合直接作为长期无人值守服务部署。

主要问题不是某一处普通内存泄漏，而是系统缺少统一的资源边界和生命周期管理：

- Gateway 为每个 TCP 连接创建一个 detached 线程，连接数和线程数没有上限；
- Agent 只有一个串行执行线程，但传输等待、请求执行和响应队列没有共享同一个截止时间与取消状态；
- Session、Skill、挂起任务分别维护容量和生命周期，长期运行后会出现状态残留及容量耗尽；
- 日志、Trace、Session 文件没有总量、文件数、TTL 或轮转策略；
- LLM、目录遍历和子进程只有局部大小限制，没有统一的 CPU、内存、时间和输出预算；
- 告警虽然标记了高优先级，但实际使用 FIFO 总线，无法获得确定性的资源预留；
- 进程缺少健康指标、故障自恢复和有截止时间的优雅退出机制。

目标方案的核心不是逐点增加判断，而是把 LiteCrab 改造成一个**有界资源运行时**：所有连接、请求、Run、Session、告警、子进程和持久化数据都由明确的 Owner 管理，并且具有容量、截止时间、取消、淘汰和故障恢复语义。

在完成第 12 章的第一阶段和第二阶段之前，不建议把当前版本作为 7×24 小时无人值守服务上线。

## 2. 设备约束与设计目标

已知 SD5091 环境：

| 项目 | 当前设备情况 |
|---|---:|
| 物理内存 | 724 MB |
| 通常可用内存 | 约 320 MB |
| 用户空间 | 32 位 ARM hard-float |
| 文件描述符上限 | 1024 |
| 可写目录 | `/mnt/home/enspire` |
| 服务管理 | BusyBox init，无 systemd |
| 本地构建能力 | 无 gcc、cmake、make、Python 3 |

建议为 LiteCrab 预留下列初始资源包络。它们是架构设计目标，最终值应通过 5091 真机压测校准：

| 资源 | 建议初始目标 |
|---|---:|
| 稳态 RSS | ≤ 32 MB |
| 单请求峰值后的进程 RSS | ≤ 96 MB |
| 进程保护性重启阈值 | 128 MB RSS |
| 常驻线程 | ≤ 8 |
| TCP 活跃连接 | ≤ 16 |
| 同时等待执行的普通请求 | ≤ 8 |
| 同时执行的重型 Agent Run | 1 |
| 文件描述符 | 正常 ≤ 64，告警阈值 128 |
| 单个 LLM HTTP 响应 | ≤ 2 MB |
| 单个 Tool 原始输出 | ≤ 256 KB；特殊 Tool 单独授权 |
| LiteCrab 总磁盘预算 | ≤ 64 MB |
| 空闲 CPU | 无告警时接近 0%，启用告警时建议 < 2% |

这些限制必须在服务内部实施。不能只依靠 `ulimit`：如果内部没有 admission control，单纯提高 `ulimit -n` 会允许更多连接和线程消耗内存；单纯降低它又可能使当前 `accept()` 在 `EMFILE` 时直接退出服务。

## 3. 当前风险清单

### 3.1 P0：无界连接线程会耗尽 32 位地址空间

当前 `StartRequestServer` 每接受一个连接就调用一次 `pthread_create`，随后 `pthread_detach`。默认接收超时为 600 秒，没有活动连接计数、每来源限制或全局连接上限。

受控 WSL 探针的结果为：

| 场景 | 线程数 | 虚拟地址空间 |
|---|---:|---:|
| 空闲服务 | 2 | 约 30 MB |
| 80 个空闲 TCP 连接 | 82 | 约 5.99 GB |
| 连接全部关闭后 | 2 | 约 5.37 GB |

该数值受 x86_64 glibc 线程栈和 arena 策略影响，不能直接套用到 ARM；但它证明了线程和虚拟地址空间与连接数线性增长，而且线程退出后分配器可能保留映射。32 位 ARM 的用户地址空间更小，实际会更早失败。

可能后果：

- `pthread_create`、`malloc` 或 TLS 分配失败；
- 内核因内存压力触发 OOM；
- 慢连接或恶意连接让服务失去接收正常请求的能力；
- 服务退出期间无法追踪和等待 detached 客户端线程。

### 3.2 P0：请求超时与执行生命周期分离，可能永久堵死 Agent

Agent 只有一个工作线程，输入和输出队列容量均为 16。TCP 调用方最多等待 600 秒，但一个 Run 可以包含 16 轮 LLM/Tool 迭代，每次 LLM 又可能发生多次网络尝试，因此执行时间可能超过调用方等待时间。

当 TCP 调用方已经超时，Agent 仍会继续执行并把结果压入全局输出队列。结果不再有消费者时会成为孤儿响应。孤儿响应达到 16 个后，唯一 Agent 线程会阻塞在 `MessageBusPushOutbound`，服务从此不能处理任何请求。

这不是简单地扩大队列可以解决的问题。根因是：

- transport timeout、queue timeout、run deadline 和 LLM/tool timeout 不是同一个 deadline；
- 请求没有可查询的生命周期状态；
- 客户端断开或超时不会取消排队任务，也不会通知执行器；
- 响应使用可能永久阻塞的全局队列，没有明确 Owner。

### 3.3 P0：磁盘使用和闪存写入无界

当前日志和 Trace 均以 append 模式写入，没有轮转、保留时间或总容量上限。每条日志会立即 `fflush`。Trace 当前默认记录完整 LLM 输入，其中可能包含 system prompt、历史消息和 Tool 结果。

当前开发工作区已经有：

- 27 个日志/Trace 文件，合计约 19.9 MB；
- 最大单个 Trace 约 1.14 MB；
- 515 个 Session 文件，合计约 32.4 MB，平均约 63 KB。

每轮 Session 保存会重写消息、Working Memory 和路由状态，并执行 `fsync`。长期运行会同时带来：

- 可写分区耗尽；
- 高频元数据更新和闪存写放大；
- 磁盘满后 Session、日志和业务 Tool 写入同时失败；
- Trace 中敏感输入长期残留。

### 3.4 P1：Session 与 Skill 生命周期不一致

Session 缓存有 32 个槽位并支持 LRU 选择，但 Skill 状态又独立维护 32 个槽位，后者没有 LRU、TTL 或随 Session 淘汰自动释放的机制。网络连接关闭执行的是 Session 引用释放，并不等价于关闭持久 Session，也不会清理对应 Skill 状态。

可能后果：

- 累计 32 个不同 Session 使用 Skill 后，新 Session 无法再建立 Skill 状态；
- 全局挂起队列只有 16 项，旧挂起任务可能长期占位；
- Session 文件已淘汰但 Skill 状态仍驻留，形成跨模块状态不一致；
- Run Store 全部被非终态 Run 占用时，新 Run 可能失去完整跟踪。

### 3.5 P1：CPU 和内存峰值缺少统一预算

当前存在多个局部上限，但它们没有组成进程级预算：

- LLM HTTP 响应可增长到 16 MB；chunked 解码期间旧响应和新缓冲区同时存在；
- SSE 不是边收边消费，而是先缓存整个 HTTP 响应再解析；
- 流式 Tool 参数每个分片都 `realloc`，大参数可能出现 O(n²) 拷贝和堆碎片；
- `grep`/`glob` 没有扫描文件数、累计字节数和执行时间限制，达到结果上限后仍可能继续遍历；
- `exec_program` 有 wall-clock timeout，但没有 CPU、地址空间、进程数、FD 和输出文件大小限制；
- 每轮 Agent 调用栈同时包含多组 64 KB～128 KB 缓冲区；
- 告警服务预分配 64 个较大的 Delivery，解析时还会创建较大的临时栈和 token 缓冲区。

### 3.6 P1：告警没有真正的优先级和可靠投递

告警消息设置为高优先级，但当前 MessageBus 是单一 FIFO 队列，未读取 `priority` 字段。因此普通请求排队时，告警不能插队，也没有预留容量。

当前投递语义是在告警进入 Agent 输入队列后立即视为 forwarded：

- 进程在真正处理前崩溃会丢失该告警；
- Agent 长任务会延迟告警处理；
- 告警风暴会与普通请求竞争仅有的 16 个槽位；
- 去重信息仅在进程内保存，重启后活动告警会重新投递。

此外，dispatcher 空闲时每 25 ms 唤醒一次，约 40 次/秒。它不是主要资源风险，但不符合端侧常驻进程应采用事件驱动等待的原则。

### 3.7 P1：故障恢复和可观测性不足

当前没有以下能力：

- 活跃连接、排队请求、执行 Run、孤儿响应、Session 数量等运行指标；
- RSS、线程、FD、磁盘预算的内部告警；
- 本地 health/readiness 状态；
- 卡住 Run 的 watchdog；
- 有截止时间的 shutdown coordinator；
- BusyBox 环境下经过验证的自动重启和退避策略。

现有启动脚本将进程放到后台，但不等价于持续监督。进程崩溃、OOM 或异常退出后可能一直保持离线。

### 3.8 P2：当前 ARM 构建产物可能不是最新源码

当前 `build-5091/litecrab_server` 约 4.2 MB，静态段信息为：

- text：约 3.13 MB；
- data：约 230 KB；
- bss：约 3.31 MB。

但现有 ARM 二进制的时间早于部分源码修改时间。部署流程需要由构建产物清单和源码 revision 保证一致性，不能人工根据文件是否存在判断是否可部署。

## 4. 总体架构原则

目标架构采用以下原则：

1. **所有资源有界**：连接、请求、线程、Run、Session、磁盘、响应、子进程均有硬上限。
2. **所有异步对象有 Owner**：创建者、完成者、取消者和销毁者必须明确。
3. **一个请求只有一个 deadline**：Gateway、Scheduler、LLM、Tool 和响应发送都从同一绝对截止时间派生剩余时间。
4. **背压优先于堆积**：容量不足时尽早返回 busy/retry-after，不能无限创建线程或扩大队列。
5. **控制面与数据面分离**：连接 I/O、Run 调度、Agent 执行、持久化和日志不互相阻塞。
6. **端侧默认降级**：资源不足时拒绝新任务、缩减 Trace、暂停非关键维护，优先保留告警和健康检查。
7. **故障是正常状态**：DNS、LLM、磁盘、PLC、子进程都可能失败，失败路径也必须有界。

## 5. 目标架构

```text
TCP / Local Proxy
        |
        v
+-----------------------+
| Gateway Reactor       |  1 thread, non-blocking I/O
| Connection Table      |  max 16, idle/deadline enforcement
+-----------+-----------+
            |
            v
+-----------------------+       +----------------------+
| Request Registry      |<----->| Response Sink        |
| state/deadline/cancel |       | connected/discarded  |
+-----------+-----------+       +----------------------+
            |
            v
+-----------------------+
| Bounded Scheduler     |
| alarm/control/normal  |  reserved capacity + admission control
+-----------+-----------+
            |
            v
+-----------------------+
| Run Coordinator       |  1 heavy Run on SD5091
| Resource Budget       |  tokens/time/memory/I/O/tool calls
+-----+-------------+---+
      |             |
      v             v
 LLM Stream      Executor Helper
 incremental     rlimit/process group/output cap
 parser
      |
      v
+-----------------------+       +----------------------+
| Unified Session Store |<----->| Storage Manager      |
| session+skill+memory  |       | quota/TTL/checkpoint |
+-----------------------+       +----------------------+
            |
            v
+-----------------------+
| Bounded Log/Trace Sink|  rotation, redaction, batching
+-----------------------+

Alarm Collector -> Durable Alarm Spool -> reserved Scheduler lane

Supervisor/Watchdog observes every component and owns shutdown/restart.
```

## 6. Gateway 与请求生命周期重构

### 6.1 使用 Reactor 或固定线程池，取消一连接一线程

SD5091 的协议是简单的 TCP line framing，适合使用一个非阻塞 Reactor：

- 一个监听 socket；
- 一个 poll/epoll 线程；
- 固定大小的 Connection Table，建议初始值 16；
- 每连接固定或按需分配的有界输入缓冲区；
- 输出采用有界发送缓冲，并由 Reactor 处理部分写；
- 超过容量时 accept 后立即返回 busy 并关闭，或由上游代理得到明确的 overload 响应。

如果短期不引入非阻塞 I/O，也应使用固定大小的 4～8 线程连接池，而不是 detached thread。线程必须由 Server 对象持有并在退出时 join。

### 6.2 引入 Request Registry

每个请求进入系统后建立 `RequestContext`：

```c
typedef struct {
    RequestId id;
    RequestClass class;
    RequestState state;
    int64_t acceptedAtMs;
    int64_t deadlineMs;
    CancellationToken cancel;
    ResponseSink sink;
    SessionHandle session;
    ResourceBudget budget;
} RequestContext;
```

状态机建议为：

```text
ACCEPTED -> QUEUED -> RUNNING -> COMPLETED
    |          |         |
    +----------+---------+-> CANCELLED / TIMED_OUT / FAILED
```

约束：

- 状态只能单向转换；
- 客户端断开时将 `ResponseSink` 标记为 detached，并取消仍在排队的普通请求；
- 已执行请求在安全点检查 CancellationToken；
- 完成结果直接交给所属 ResponseSink，不进入可能无限等待的全局输出队列；
- Sink 已失效时，普通响应直接释放；需要异步查询的响应显式进入有配额的 Result Store；
- 任何完成路径都必须从 Registry 删除并释放 Session 引用。

### 6.3 使用统一绝对 deadline

请求在 Gateway 被接受时就计算 `deadlineMs`。后续组件不得重新给出完整超时时间，而应使用：

```text
remaining = request.deadline - monotonic_now
```

LLM connect/read、重试退避、Tool timeout、排队等待和响应发送均不得超过 remaining。重试前若剩余预算不足，直接终止，不再开始新尝试。

所有运行时 deadline 使用 `CLOCK_MONOTONIC`。墙上时间只用于日志和持久化元数据，避免设备校时导致超时突然延长或提前。

## 7. 有界 Scheduler 与服务等级

### 7.1 按工作负载分通道

建议设置三个逻辑队列：

| 队列 | 用途 | 建议容量 | 行为 |
|---|---|---:|---|
| control | health、shutdown、取消 | 4 | 永远保留，不能被业务占满 |
| alarm | 活动告警 | 4～8 | 有持久 spool，允许合并 |
| normal | 用户请求 | 8 | 满时立即返回 busy |

Scheduler 使用加权轮询或严格优先级加防饥饿机制，例如每处理一个 alarm 后最多连续处理两个 normal。`LitePriority` 必须参与真实调度，而不只是消息元数据。

### 7.2 重型执行并发保持为 1

在 SD5091 上，LLM 响应缓冲、TLS、Agent 栈和 Tool 子进程都会产生明显峰值。初始版本应保持一个重型 Run 并发，以换取可预测内存。

吞吐通过以下方式改善，而不是盲目增加 Agent 线程：

- 排队和执行解耦，避免连接线程占资源；
- LLM 真流式解析，降低首包延迟和内存；
- 持久 HTTP/TLS 连接或由 PC Transfer Station 复用连接；
- 对只读、轻量且确定性的 control 操作走独立路径；
- 对重复活动告警合并，而不是重复启动完整诊断。

## 8. Run Resource Budget

每个 Run 建立统一预算，并由可信运行时代码消费：

```c
typedef struct {
    int64_t deadlineMs;
    uint32_t llmCallsRemaining;
    uint32_t toolCallsRemaining;
    uint64_t llmResponseBytesRemaining;
    uint64_t toolOutputBytesRemaining;
    uint64_t filesystemScanBytesRemaining;
    uint32_t filesystemEntriesRemaining;
    uint32_t childProcessesRemaining;
} ResourceBudget;
```

推荐初始值：

| 预算 | 普通请求 | 告警诊断 |
|---|---:|---:|
| 总 deadline | 120 秒 | 180 秒 |
| LLM 调用 | 8 | 12 |
| Tool 调用 | 16 | 24 |
| 单次 LLM 响应 | 2 MB | 2 MB |
| 单 Tool 输出 | 256 KB | 256 KB |
| 文件扫描累计字节 | 8 MB | 8 MB |
| 文件条目 | 10,000 | 10,000 |
| 子进程 | 4 | 8 |

达到预算时返回结构化 `limit` 结果，不应继续重试，也不能把资源耗尽误报为业务成功。

### 8.1 LLM 客户端重构

LLM 客户端应改为增量 HTTP/SSE 解析：

- 只保留 HTTP header、小型接收块和累计结果，不保存完整 wire response；
- chunked 和 SSE 均采用状态机增量消费；
- 文本输出、Tool call arguments 各自有硬上限；
- Tool arguments 使用带 capacity 的增长缓冲，按倍数扩容，避免每分片 `realloc`；
- connect、TLS handshake、read idle、total deadline 分开统计；
- 网络错误使用带 jitter 的退避，但必须受总 deadline 约束；
- 明确区分完整响应、对端正常关闭、超时和截断响应。

建议将当前 16 MB wire response 上限降低到 2 MB。`max_tokens=2048` 时通常不需要 16 MB 响应；如特定 Provider 有大量 reasoning 字段，应单独测量后配置，而不是共享一个过大的默认值。

### 8.2 文件搜索重构

目录遍历器应成为共享的 `BoundedWalker`：

- 最大深度；
- 最大条目数；
- 最大累计读取字节；
- deadline/cancellation；
- 排除规则；
- 达到结果数后立即停止遍历；
- 每处理一批文件主动让出控制权并检查取消。

`grep`、`glob`、未来索引和 Skill 扫描统一复用它，避免各 Tool 分别补限制。

## 9. 子进程执行架构

不建议继续在多线程主进程中直接承担完整的 fork/exec、超时、输出读取和清理职责。目标方案增加一个受控 `Executor Helper`：

- Helper 由主进程启动并通过 Unix domain socket 或 pipe 接收执行请求；
- 主进程只提交结构化 executable、argv、cwd 和预算，不提交 shell 字符串；
- Helper 在执行前设置进程组、最小环境和资源限制；
- 子进程 stdout/stderr 有独立上限，超限立即终止进程组；
- timeout 先 SIGTERM，短暂 grace period 后 SIGKILL；
- Helper 异常退出时由 Supervisor 重建；
- 结果包含 exit、signal、timeout、limit 类型和实际资源使用。

建议初始限制：

| 限制 | 建议值 |
|---|---:|
| `RLIMIT_CPU` | 10～15 秒 |
| wall-clock | 默认 30 秒，最大 120 秒 |
| `RLIMIT_AS` | 64 MB，按 PLC 程序实测调整 |
| `RLIMIT_NOFILE` | 32 |
| `RLIMIT_NPROC` | 4 |
| `RLIMIT_FSIZE` | 4 MB |
| stdout+stderr | 256 KB |

`shell` 继续保持默认禁用。`exec_program` 采用 executable allowlist 或 Skill manifest 声明，不能因为文件位于 workspace 且可执行就自动获得运行权限。

## 10. 统一 Session、Skill 与持久化生命周期

### 10.1 使用统一 SessionManager

Session、Working Memory、Skill active state、挂起 Run 和路由状态应由同一个 SessionManager 管理：

```text
SessionRecord
  metadata / owner
  conversation
  working memory
  active skill
  bounded skill history
  pending runs
  refcount / pin count
  last access / expiry
  dirty / checkpoint state
```

这样可以建立统一不变量：

- Session 被 LRU 淘汰时，其非持久临时 Skill 状态同时释放；
- 有活动 Run 的 Session 被 pin，不能静默淘汰；
- pin 数也有上限，超过上限拒绝新 Run；
- 持久 Session 关闭、过期或删除时，相关挂起 Run 一并进入 cancelled；
- 不再维护互相不知道对方生命周期的多个固定数组。

### 10.2 磁盘配额与保留策略

5091 不需要引入重量数据库。可继续使用固定头二进制 Session 文件，但增加 Storage Manager：

- 启动时扫描 Session 目录，建立轻量索引；
- Session 文件数上限建议 128；
- Session 总预算建议 32 MB；
- 非 pin Session 默认 TTL 建议 7 天，可配置；
- 超额时先删除已过期，再按最后访问时间淘汰；
- 临时 Session 不落盘，只有客户端提供稳定 `sessionId` 时持久化；
- 启动时清理遗留 `.tmp.<pid>` 文件；
- 写入使用 atomic rename，并对目录进行必要的 durability 处理。

### 10.3 降低闪存写放大

Session 写入应采用 checkpoint 策略：

- completed/waiting-input 等关键状态立即 checkpoint；
- 同一 Run 内的普通 dirty 更新合并；
- 每个 Session 设置最短 checkpoint 间隔；
- 关机前在 shutdown deadline 内批量 flush；
- 日志记录 checkpoint 失败，但磁盘满时不能无限重试。

如果业务要求断电后保留每一步 Tool 状态，应增加小型 append-only journal，再异步压缩为 Session 快照，而不是每一步重写约 60 KB 文件。

## 11. 日志、Trace、告警和监督

### 11.1 Bounded Log Sink

日志和 Trace 统一交给单写线程：

- 业务线程只向有界内存队列提交结构化事件；
- 批量写入，按时间或缓冲大小 flush；
- error/critical 可要求立即 flush；
- 队列满时优先保留 error、资源告警和生命周期事件，丢弃或采样 debug；
- 记录 dropped event 计数，不能静默丢失。

建议磁盘策略：

| 类型 | 单文件 | 文件数 | 总预算 |
|---|---:|---:|---:|
| 主日志 | 2 MB | 4 | 8 MB |
| Trace | 2 MB | 4 | 8 MB |
| 告警 spool | — | — | 8 MB |

完整 LLM 输入默认关闭。只有临时诊断时通过配置开启，并设置自动失效时间。常态 Trace 只保存长度、hash、token、状态、错误码和经过脱敏的摘要。

### 11.2 Durable Alarm Spool

告警应先写入一个小型、有界、可恢复的 spool，再提交 Scheduler：

```text
RECEIVED -> QUEUED -> RUNNING -> ACKED
                         |
                         +-> RETRY / DEAD_LETTER
```

设计要求：

- `equipid + almid + seqno + localtime` 作为幂等键；
- 只有 Agent 处理完成或明确接受异步处理后才能 ACK；
- 进程重启后恢复未 ACK 项；
- 同一设备、同类活动告警可合并为摘要，防止告警风暴；
- alarm 队列拥有独立预留容量；
- spool 达到预算时保留最新关键告警并产生明确的 overflow 事件；
- dispatcher 使用 condition variable 等待最近 due time，不固定每 25 ms 唤醒。

### 11.3 Supervisor 与健康状态

进程内 Supervisor 维护：

- Reactor、Scheduler、Agent、Alarm、Log Sink、Executor Helper 的心跳；
- 当前 RSS、线程、FD、队列、Session、磁盘使用；
- 最近成功 LLM、告警查询、Session checkpoint 时间；
- 连续失败计数和 degraded 原因。

本地 health 状态至少分为：

- `ready`：可接收新普通请求；
- `degraded`：可服务但 LLM、告警或磁盘接近阈值；
- `overloaded`：拒绝普通请求，保留 control/alarm；
- `stopping`：不再 admission，新请求明确失败。

BusyBox init 使用经过验证的 respawn 配置或外部 PC watchdog。重启策略必须带退避，防止配置错误导致高频重启和日志写爆。

### 11.4 有截止时间的退出

Shutdown Coordinator 按顺序执行：

1. Gateway 停止 admission；
2. Alarm Collector 停止接收新事件；
3. 取消排队的普通请求；
4. 给当前 Run 一个有限 grace period；
5. 终止 Executor 子进程组；
6. checkpoint 关键 Session 和 alarm spool；
7. flush Log Sink；
8. join 所有被跟踪线程；
9. 超过总 shutdown deadline 时快速退出，由 Supervisor 记录非优雅关闭。

不得再使用无法追踪的 detached 工作线程。

## 12. 实施路线

### 阶段 0：部署前遏制措施

该阶段不改变核心架构，只降低当前版本暴露面：

- 仅监听 loopback；
- 由 PC Transfer Station 或本地代理限制并发连接和速率；
- 将 TCP 空闲超时从 600 秒降低到约 30 秒；
- 设置 `LITECRAB_TRACE_LOG_LLM_INPUT=0`；
- `max_tokens` 使用 1024～2048；
- 外部监控 RSS、VmSize、线程、FD、日志和 Session 目录；
- 设置磁盘清理和进程自动拉起；
- 每次部署重新交叉编译，并验证源码 revision、ELF32 ARM 和全静态链接。

阶段 0 只能减轻风险，不能解决孤儿响应死锁、Skill 容量耗尽等结构性问题。

### 阶段 1：建立有界运行时主干

优先完成：

1. Gateway Reactor 或固定连接池；
2. Connection Table 和 admission control；
3. Request Registry、统一 deadline 和 cancellation；
4. 分级有界 Scheduler；
5. 请求所属 ResponseSink，删除阻塞式全局输出队列语义；
6. 所有线程可追踪、可 join；
7. 基础资源指标和 health 状态。

完成标准：慢连接、客户端断开和请求超时均不能增加长期线程/FD，也不能堵死 Agent。

### 阶段 2：限制执行峰值

1. 增量 HTTP/chunked/SSE parser；
2. Run Resource Budget；
3. BoundedWalker；
4. Executor Helper + rlimit + process group；
5. LLM/tool 重试统一受总 deadline 约束；
6. ARM 真机测量 stack high-water mark，再显式设置各线程栈大小。

完成标准：所有输入规模和故障路径均有确定的内存、CPU 和时间上界。

### 阶段 3：统一状态和磁盘治理

1. 统一 SessionManager；
2. Session/Skill/Run 生命周期一致化；
3. TTL、LRU、pin 和磁盘配额；
4. checkpoint 合并或 journal；
5. Bounded Log Sink 和轮转；
6. 磁盘满降级策略。

完成标准：创建超过 1000 个 Session 或持续写日志时，内存槽位和磁盘仍保持在预算内。

### 阶段 4：告警可靠性与无人值守运行

1. Durable Alarm Spool；
2. 告警预留调度通道；
3. 告警合并、重试和 dead-letter；
4. Supervisor、watchdog 和有界 shutdown；
5. BusyBox init 自动恢复和升级回滚流程。

完成标准：Agent 忙、进程重启、LLM 离线和告警风暴时，关键告警仍可恢复、可审计且不会拖垮设备。

## 13. 可作为局部修复处理的问题

以下问题不会改变核心对象所有权，可以独立修复：

- Trace 注释称默认关闭，但变量当前默认开启：直接改为默认关闭；
- Alarm dispatcher 固定 25 ms 唤醒：改为按最近任务 due time 条件等待；
- 总线 timed wait 使用墙上时间：改为 monotonic condition variable；
- ARM 构建产物缺少 revision：构建时写入版本并生成 manifest；
- `grep` 当前分配 64 KB 输出但实际只给 append 4 KB 容量：修正为一致的配置值；
- 日志目录和 Session 目录启动时没有显式报告容量：增加一次启动检查。

这些小修可以提前合入，但不能替代第 12 章的架构重构。

## 14. 验证计划与上线门槛

### 14.1 自动化测试

必须新增：

- 连接 churn：至少 100,000 次连接/断开，线程和 FD 回到基线；
- slowloris：连接占满时正常请求得到确定的 overload，而非 OOM；
- timeout orphan：连续制造超过队列容量的超时请求，Agent 仍能处理后续请求；
- cancellation：排队、LLM、Tool、发送阶段分别取消；
- Session churn：超过 1000 个 Session 和 Skill Run 后仍可创建新状态；
- disk quota：日志、Trace、Session、spool 达到上限后保持总量不增长；
- LLM fault：DNS 失败、连接超时、429、503、partial header、partial chunk、缺少 `[DONE]`；
- Tool fault：输出洪泛、fork 失败、超时、孙进程、忽略 SIGTERM；
- clock change：修改墙上时间不影响 deadline；
- shutdown：每个阶段卡住时仍在总 deadline 内退出；
- power-loss：在 Session/spool checkpoint 各阶段 kill -9 后可以恢复。

### 14.2 5091 真机 soak

至少执行一次 72 小时测试：

- 告警轮询/订阅持续开启；
- 周期性正常请求、LLM 超时和 Tool 执行；
- 周期性连接 churn；
- 模拟网络断开和恢复；
- 每分钟采集 RSS、VmSize、线程、FD、CPU、磁盘、队列长度和请求延迟。

建议上线判定：

- warm-up 后 RSS 增长斜率小于 1 MB/24h；
- 线程、FD 和 Connection Table 无持续增长；
- 磁盘始终受配额约束；
- 空闲 CPU 满足第 2 章目标；
- 没有请求可让 control/health 通道失效；
- LLM 离线 24 小时不会造成队列、日志或重试风暴；
- 重启后未完成告警可恢复，Session 文件无批量损坏。

## 15. 运行监控最小集合

在完整 metrics 接口实现前，外部 watchdog 至少采集：

```sh
pid="$(pidof litecrab_server)"
grep -E 'VmRSS|VmSize|Threads' "/proc/$pid/status"
ls "/proc/$pid/fd" | wc -l
du -sk logs .crab/sessions
```

建议初始告警条件：

- RSS > 96 MB 或连续 6 小时单调增长；
- Threads > 12；
- FD > 128；
- LiteCrab 数据目录超过 48 MB；
- 10 分钟内无健康心跳；
- 连续 LLM 失败或告警查询失败超过配置阈值；
- 请求队列连续 60 秒处于满状态。

## 16. 最终建议

本次整改应先建立“Gateway → Request Registry → Scheduler → Run Coordinator → Response Sink”这条有界主干，再统一 Session/Skill/Storage 生命周期。日志轮转、子进程隔离、告警 spool 和 Supervisor 都应挂在这套生命周期与预算模型上。

不建议采用以下方式作为最终方案：

- 仅扩大消息队列；
- 仅提高 `ulimit -n`；
- 仅缩短 TCP timeout；
- 仅定时重启掩盖内存和状态残留；
- 为 Session、Skill、Alarm 分别增加更大的固定数组；
- 在每个 Tool 内各自实现不一致的超时和大小判断。

这些措施最多作为临时遏制，无法建立可证明的长期运行边界。目标应是：即使客户端、LLM、网络、磁盘或 Tool 行为异常，LiteCrab 仍能在固定资源包络内拒绝、降级、恢复和退出。
