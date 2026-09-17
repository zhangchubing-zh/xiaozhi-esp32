# 历史版本功能同步审计

对比来源：`https://github.com/zhangchubing-zh/LiteCrab` 的 `main` 历史版本。同步原则是保留功能语义，基于当前 `docs/` 架构重新设计实现，不复制旧源码。

| 历史能力 | 对比结果 | 当前处理 |
|---|---|---|
| Runtime 工具、Hub、TCP Gateway、LLM Tool Adapter、Trace | 当前实现已覆盖且边界校验更完整 | 不重复同步旧实现 |
| 基础配置与多 LLM Provider 配置 | 原先缺少 | 已新增严格 JSON 加载、默认 Provider、环境变量和 CLI 覆盖 |
| TCP 纯文本请求 | 原先只接受 JSON | 已兼容纯文本和 JSON，JSON 路径继续严格校验 |
| Working Memory / Workflow | 原先缺少结构化状态 | 已新增任务、目标、命令、状态、路径、步骤、选项、最近 8 次工具历史、已加载 Skill 缓存和 Prompt 注入 |
| PC Transfer Station | 原先缺少 | 已用 Python 标准库重新实现 TCP 板端代理、LLM HTTP 代理、健康/状态、CORS、校验、重试、轮转日志、信号退出和线程健康监控 |
| POSIX/静态构建策略 | 原先不完整 | 原生 Windows 明确拒绝并提示 WSL；静态链接仅在静态 OpenSSL 可用时开放并于配置期失败提示 |
| LLM 简单 Prompt、回调、last-error 包装 API | 仅是旧 LLM 调用的便利包装，不产生独立产品能力 | 不同步旧 API；统一使用 `LlmChatToolsEx` 返回值、响应对象和 Trace |
| 旧版硬编码第三方依赖及宽松输入处理 | 有安全和维护风险 | 不同步；使用已有严格 JSON、路径包含校验、TLS 主机名/证书校验和有界缓冲区 |
| 配置内明文密钥 | 历史仓库存在敏感值 | 不同步任何值；示例保持空密钥并推荐环境变量注入 |

## 验证范围

- C Runtime/Hub/Session/Skill/LLM 解析及 Working Memory 单元测试；
- JSON 与纯文本 TCP 请求到 Mock OpenAI SSE、再到真实工具调用的端到端测试；
- Transfer Station 的 TCP、HTTP、健康检查、默认模型和错误输入集成测试；
- WSL Release 编译，以及 AddressSanitizer、UndefinedBehaviorSanitizer 和泄漏检测。
