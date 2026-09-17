# LiteCrab Trace Viewer（浅色主题版）

一个纯前端、零依赖的可视化工具，用于浏览 `logs/agent_trace_*.jsonl` 轨迹文件，
还原 Agent 的执行流程、关键事件、工具调用与异常。

> 本版本采用 **浅色（亮色）主题**，结构与功能与项目根目录下的 `trace_viewer/`（深色主题）完全一致，仅视觉风格不同。如需切换深色主题，使用项目根目录下的 `trace_viewer/index.html`。

## 用法

### 方式一：直接打开

直接双击 `index.html`，在浏览器中打开。然后：

1. 把 `logs/` 下的 `agent_trace_*.jsonl` 文件拖入页面，或点击「选择文件」/「选择 logs 目录」。
2. 所有解析都在浏览器本地完成，不上传任何数据。

### 方式二：拖目录

点击「选择 logs 目录」选中整个 `logs` 目录，可一次性加载所有 trace 文件。

> 推荐用 Chrome / Edge / Firefox。文件较大时建议一次加载 20 个以内，否则浏览器渲染会变慢。

## 页面结构

### 顶部统计栏
打开文件后，顶部会显示一组卡片，包含：

| 指标 | 含义 |
|------|------|
| Trace 文件 | 加载的 `.jsonl` 文件数 |
| Sessions | 不同 sessionId 数量 |
| 用户数 | 不同 userId 数量 |
| 总请求数 | trace_start 事件总数 |
| 成功 / 失败 | status=ok / 含异常的请求数及成功率 |
| LLM 调用 | 累计 llmCalls |
| 工具调用 | 累计 toolCalls |
| 异常事件 | anomaly 事件总数 |
| Token 总量 | prompt + completion，并分别显示上下行 |
| Agent 处理耗时 | 所有 trace 的 durationMs 之和 |
| 日志时间跨度 | 最小/最大 timeMs 区间 |

### 左侧 Sessions 列表
- 按 sessionId 分组，按首次出现时间排序。
- 每条显示 sessionId、状态（OK / 有异常）、请求数、工具数、LLM 数、异常数、起始时间。
- 顶部搜索框可按 sessionId / userId 过滤。

### 右侧执行流程
选定 session 后，右侧依次展示：

1. **Session 概览**：sessionId、用户、时间范围、统计卡片。
2. **请求时间轴**：把该 session 内每个 trace 渲染为时间条，宽度对应该次请求的处理时长。点击任意条即可展开对应 trace。
3. **执行流程列表**：所有 trace 按时间顺序排列。每张卡片可展开，内部按时间线展示：
   - 用户输入（点击查看全文）
   - 中间事件（TOOL / ANOMALY / SKILL / LOG / LLM …），每个事件可展开看输入输出，双击用 Modal 看完整原始 JSON
   - 最终输出（点击查看全文）

### 顶部过滤
- **搜索**：在 traceId / 输入 / 输出 / 工具名 / 异常详情 / 日志消息中全文搜索。
- **仅异常**：只看含 anomaly 的 trace。
- **仅含工具调用**：只看 toolCalls > 0 的 trace。
- **仅慢请求（>3s）**：只看 durationMs > 3000 的 trace。

## 支持的事件类型

源自 `docs/observability/observability_agent_trace.md`：

| 事件 | 字段 | 渲染 |
|------|------|------|
| `trace_start` | traceId, rootSpanId, userId, sessionId, input | Trace 卡片标题与输入框 |
| `trace_end` | status, finalOutput, durationMs, promptTokens, completionTokens, llmCalls, toolCalls | Trace 卡片徽章与输出框 |
| `tool` | toolName, input, output, exitCode, status | 事件流中的 TOOL 节点 |
| `anomaly` | category, level, detail | 事件流中的 ANOMALY 节点 |
| `log` | level, component, message | 普通 LOG；`component=skill` 的状态变化显示为 `SKILL_STATE` |
| `component=skill_router` | message 中的 event/action/skill/confidence/reason/session_id | 独立 `SKILL_ROUTE` 节点；数字 action 映射为 none/select/resume/clarify/error |
| `skill_*` / `skill_event` | 通过 component=agent 的日志推断 | `SKILL_LIFECYCLE` 节点 |
| `llm` | model, iteration, status, toolUse, toolNames, output, token 数；可选 `input`（见下） | `LLM` 模型决策节点 |

#### LLM 事件的输入与输出

`llm` 事件默认只记录 LLM 的**返回内容**（`output` 字段）。若需要同时看到**发给 LLM 的完整内容**，在启动 `litecrab_server` 前设置环境变量：

```bash
export LITECRAB_TRACE_LOG_LLM_INPUT=1   # 也可用 true/yes/on
```

启用后，每条 `llm` 事件会额外携带 `input` 字段，记录与 `LlmChatToolsEx` 实际发送给 LLM 完全一致的 messages 数组，包括：

- **系统提示词**（system prompt）——作为数组第一条 `{"role":"system","content":"..."}`
- **历史会话**（用户历史请求、LLM 历史回复、工具调用结果）—— `{"role":"user"/"assistant"/"tool","content":"..."}`
- **当前用户请求** —— 最后一条 `{"role":"user",...}`

也就是说 `input` 字段还原了 LLM 收到的完整上下文。在执行流程中双击 LLM 事件，弹窗会并排展示「发给LLM的内容」与「LLM返回的内容」；未启用时只显示返回内容，并提示如何开启。

> 注意：messages 可能包含敏感信息（如系统提示词、用户输入），仅建议在调试环境启用。展示层会对 token / apiKey / password / cookie / Authorization 做遮罩，但原始 JSONL 文件不修改。
| `span_*` / `user_input` | 原始字段 | 通用节点 |

工具输出按 LiteCrab 的格式（`tool:` / `success:` / `exit_code:` / `summary:` / `content:` / `truncated:`）解析，单独提取 `content` 部分展示，原始文本保留可双击查看。

Viewer 会从 Skill 日志 message 中提升 `session_id`、`skill_run_id`、`interruption_id`、`correlation_token`，避免并发 Trace 依赖时间窗口猜测归属。展示层会遮罩 token、API key、password、cookie 和 Authorization；原始 JSONL 文件不会被修改。

进程启动/关闭等没有 Session 的日志保留在“未归属日志”诊断桶中，但不会计入 Sessions、用户、请求、Token 或耗时等业务聚合指标。

## 文件结构

```
trace_viewer/
├── index.html      # 页面结构
├── trace_model.js  # Skill/Router 字段归一化与敏感信息遮罩
├── viewer.css      # 暗色主题样式
├── viewer.js       # 解析 + 渲染逻辑（纯原生 JS，无依赖）
└── README.md       # 本文件
```

## 开发建议

- 想换配色：修改 `viewer.css` 顶部 `:root` 里的 CSS 变量。
- 想新增图表：在 `viewer.js` 的 `renderStats()` / `renderSessionHeader()` 中追加 `<canvas>`，引入 Chart.js 即可。
- 想接后端：把 `loadFiles()` 改成 `fetch('/api/traces?file=...')` 取 JSONL 文本即可，其余流程不变。
