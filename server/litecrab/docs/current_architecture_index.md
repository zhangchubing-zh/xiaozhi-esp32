# LiteCrab 当前架构文档索引

本目录以当前代码为准，说明现行模块架构、子功能实现方式、执行流、使用入口和已知限制。历史实现记录保存在 `history/`，ASP 接口原始资料保存在 `ASP/`；这两个目录不参与现行架构同步。

## 总体入口

- [`architecture_documentation_specification.md`](architecture_documentation_specification.md)：模块架构文档必须遵守的内容结构、功能点展开方式和审查标准。
- [`agent_framework_design.md`](agent_framework_design.md)：进程内总体模块架构和端到端执行流。
- [`deployment/sd5091_current_implementation_guide.md`](deployment/sd5091_current_implementation_guide.md)：本次 Step 0～4 功能、配置、部署、使用和剩余问题总览。
- [`deployment/sd5091_long_running_architecture.md`](deployment/sd5091_long_running_architecture.md)：SD5091 长期运行目标架构。

## 目标方案（尚未实现）

- [`deployment/mcp_dual_mode_agent_architecture.md`](deployment/mcp_dual_mode_agent_architecture.md)：基于标准 MCP Host/Client/Server 分层的设备接入目标架构，覆盖标准 HTTP 设备、反向连接设备和无云部署。
- [`deployment/mcp_dual_mode_agent/README.md`](deployment/mcp_dual_mode_agent/README.md)：MCP 角色部署、协议兼容、能力目录、执行路由、设备注册、安全、运维和测试子功能文档集。

## 当前模块文档

| 模块 | 文档 | 核心职责 |
|---|---|---|
| 配置与启动 | [`config/config_module_architecture.md`](config/config_module_architecture.md) | 默认值、配置文件、环境变量、CLI、启动与停机编排 |
| Gateway | [`gateway/gateway_module_architecture.md`](gateway/gateway_module_architecture.md) | TCP 行协议、固定连接池、Session 入口、满载拒绝 |
| Hub | [`hub/hub_module_architecture.md`](hub/hub_module_architecture.md) | Ingress、优先级队列、Request Registry、deadline/取消 |
| Kernel | [`kernel/agent_kernel_architecture.md`](kernel/agent_kernel_architecture.md) | Agent 主循环、上下文、LLM/Tool 循环、完成状态 |
| Skill | [`skill/skill_module_architecture.md`](skill/skill_module_architecture.md) | Skill 发现、路由、激活、挂起、恢复和完成 |
| Runtime | [`runtime/tool_runtime_architecture.md`](runtime/tool_runtime_architecture.md) | Tool 注册、参数校验、执行、过滤与资源边界 |
| Session | [`session/session_module_architecture.md`](session/session_module_architecture.md) | 用户归属、内存 cache、持久化、配额与 Working Memory |
| Observability | [`observability/observability_module_architecture.md`](observability/observability_module_architecture.md) | 文本日志、JSONL Trace、轮转和敏感输入控制 |
| Alarm | [`alarm/alarm_module_architecture.md`](alarm/alarm_module_architecture.md) | 告警接收、去重、durable spool、ACK、重试和恢复 |
| PLC | [`plc/plc_diagnosis_module_architecture.md`](plc/plc_diagnosis_module_architecture.md) | PLC_Diagnosis Skill、受限二进制调用和双层验证 |
| Security | [`security/security_module_architecture.md`](security/security_module_architecture.md) | 当前安全边界、已落地控制、风险和上线门槛 |
| JSON Utility | [`util/json_utility_module_architecture.md`](util/json_utility_module_architecture.md) | 严格 JSON 校验、token 导航、类型提取和有界构建 |

## 阅读规则

1. 每份模块文档按 [`architecture_documentation_specification.md`](architecture_documentation_specification.md) 展开；当前行为以模块文档、代码和自动化测试共同校验。
2. `deployment/sd5091_long_running/` 中的 Step 文件同时包含当前实现和目标差距，不能把目标方案当成已实现功能。
3. `history/` 只用于追溯，不作为现行接口依据。
4. `ASP/` 只记录设备接口资料，不描述 Agent 内部架构。
