# Step 0～4 实现状态与真机门槛

本文记录当前“可运行实现版本”与目标架构的差异，避免把 WSL 通过误写成 SD5091 已完成。目标设计仍以各阶段文档为准。

| 阶段 | 当前已经落地 | 当前实现边界 / 后续项 |
|---|---|---|
| Step 0 | loopback 默认绑定；30 秒 TCP 超时；16 KB 请求；4 worker/16 connection；Trace 输入默认关闭；revision/build time；CMake 正式安装树仅含 `bin/litecrab_server` | config/skills 由板端预置；启动/停止/退避由板端既有进程负责，管理接口只预留边界并待联调 |
| Step 1 | 可 join 固定连接池；满载 busy；停机 shutdown 活动 fd；32 槽 Request Registry；超时/迟到响应释放；control/alarm/normal 独立队列与 2:1 加权；绝对 deadline/取消检查 | 当前采用文档允许的过渡固定池，并非最终 epoll Reactor；health/metrics 的对外 control 协议待板内接口确定；旧 outbound 队列仅为 API/测试兼容保留，生产完成路径不再使用 |
| Step 2 | LLM wire 2 MB、HTTP header 32 KB；Content-Length 预拒绝；SSE 必须完整 `[DONE]`；Tool 参数倍增缓冲；LLM/Tool 次数与 deadline；Walker 条目/字节/深度/时间上限；exec CPU/AS/FD/FSIZE、256 KB 输出、进程组和 wall timeout | HTTP body 仍是 2 MB 有界整块缓存，尚未改成完全增量 SSE 状态机；exec 已有进程隔离和回收，但尚未成为常驻独立 Helper。小值 `RLIMIT_NPROC` 已因共享 UID 语义错误而移除 |
| Step 3 | Session 128 文件/32 MB/7 天 TTL；临时文件清理；LRU 回收；原子写、文件及目录 fsync；连接临时 session 不落盘；日志/Trace 2 MB × 4 轮转和批量 flush | 日志当前为同步有界批处理，不是独立 Log Sink 线程；Session 与 Skill 的所有生命周期索引尚未完全合并为单一 Storage Manager |
| Step 4 | alarm 预留通道；先 durable spool 后调度；非 ACK 重启恢复；完成状态 `HANDLED/RETRYABLE/PERMANENT/CANCELLED`；`HANDLED` 两阶段 replay barrier，ACK 落盘中断后不重跑 Skill；受限重试和持久 dead-letter；条件变量按 due time 唤醒；停机取消活动告警 Run | spool 当前是固定 64 条的版本化快照（远小于 8 MB），还没有同设备风暴摘要、overflow health 指标和人工 dead-letter 管理 API；进程内完整 Supervisor 仍待 health/metrics 接口落地；进程外恢复属于板端既有 Owner |

## 已验证边界

- WSL 原生 Debug/Release 构建、`-Wall -Wextra -Wpedantic -Werror` 构建通过。
- ASan + UBSan 覆盖单元、E2E、告警恢复和连接压力；GCC `-fanalyzer` 的生产代码告警已清零。
- 100,000 次连接 churn 后线程、FD、RSS 回到预算；28 个 slow connection 下最多保留 16 个，其余明确 busy。
- 告警在 Agent 未完成时停止并重启，可以从 spool 恢复；处理完成后不会再次恢复；可重试失败耗尽后持久保留为 dead-letter；spool 不可写时不向 Agent 转移责任。
- Tool 输出洪泛在 256 KB 终止整个进程组；忽略 SIGTERM 的程序在 wall deadline 后被 SIGKILL 回收。
- 10,000 组确定性畸形告警 JSON 已在 Sanitizer 下执行。
- 单二进制部署契约测试验证：安装树只包含 `bin/litecrab_server`；程序从安装树外读取板端 config/workspace/skills 并可有界退出，启动前后不向安装树复制文件。
- arm-linux-gnueabihf 静态交叉构建通过：唯一产物为 `litecrab_server`，ELF32、ARM、EABI5 hard-float、无 `NEEDED`；SHA-256 仅作为开发侧验证证据。

## 必须留到 SD5091 的验收

1. PLC Diagnosis 二进制在 `RLIMIT_AS=64 MB`、CPU 15 秒和 256 KB 输出限制下的真实高水位与功能回归。
2. 板上 glibc/NSS、DNS、TLS 证书、时钟校准及静态链接运行兼容性。
3. kill -9/断电发生在 spool 和 Session 每个 fsync/rename 点时的恢复结果。
4. 板内现有进程管理器接管 `status/start/stop/restart`，PC 仅调用该接口。
5. 告警风暴、LLM 长时间离线、磁盘满和 72 小时 soak；确认 RSS、FD、线程、磁盘及最老未 ACK 年龄稳定。
