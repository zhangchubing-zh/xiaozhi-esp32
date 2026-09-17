# observability_agent_trace：Agent 执行链路追踪

## 1. 模块职责

`observability/agent_trace` 提供基于 **OpenTelemetry Span 模型** 的 Agent 执行链路追踪，把一次用户请求的完整执行过程（LLM 调用、工具调用、Skill 生命周期、异常、日志）记录为 **JSONL 文件**，供离线分析（`scripts/agent_log_trace_viewer` 可视化）。

核心概念：
- **Trace**：一次用户请求的完整链路，用 `AgentTraceScope` 承载。
- **Span**：一次 Agent 动作（LLM 调用 / 工具调用 / Skill 执行），用 `AgentTraceSpan` 承载。
- **SkillEvent**：Skill 生命周期事件（start/step/end/resume…）。

## 2. 关键数据结构

### 2.1 Trace 作用域（AgentTraceScope）

```c
typedef struct AgentTraceScope {
    char traceId[64];            /* 全链路唯一标识 */
    char rootSpanId[64];         /* 根 Span 标识 */
    long long startTimeMs;
    unsigned int systemPromptHash;   /* 首次记录 system prompt 的哈希 */
    int systemPromptRecorded;        /* 是否已记录 system prompt */
    int lastMessageCount;            /* 上次记录 messages 条数（增量） */
    char skillSpanId[64];            /* 活跃 skill 的 Span ID */
    int totalPromptTokens;
    int totalCompletionTokens;
    int llmCallCount;
    int toolCallCount;
} AgentTraceScope;
```

### 2.2 Span（AgentTraceSpan）

```c
typedef struct AgentTraceSpan {
    char spanId[64];
    char traceId[64];
    char parentSpanId[64];
    char type[32];      /* llm / tool / skill ... */
    char name[96];
    long long startTimeMs;
} AgentTraceSpan;
```

### 2.3 Skill 事件（AgentTraceSkillEvent）

携带 eventType / skillRunId / sessionId / requestId / name / step / exitCode / status / steps / llmCalls / phase / outcome / spanId / fromState / toState / signal / stateType 等字段，覆盖 start / step / end / resume / state_transition 等多种事件。

### 2.4 枚举

```c
typedef enum { AGENT_TRACE_LLM_FULL, AGENT_TRACE_LLM_INCREMENTAL } AgentTraceLlmMode;
typedef enum { AGENT_TRACE_ANOMALY_WARN, AGENT_TRACE_ANOMALY_ERROR } AgentTraceAnomalyLevel;
```

- `LLM_FULL`：全量记录 prompt 与 messages。
- `LLM_INCREMENTAL`：增量记录 messages，prompt 引用首次记录。

## 3. 输出文件

- 文件名：`agent_trace_YYYYMMDD_HHMMSS.jsonl`（同名冲突追加 `_NNN` 后缀）。
- 目录：`AgentTraceInit(logDir)` 指定的目录。
- 每行一个 JSON 对象（事件），`fprintf + fflush` 即时落盘。

### 3.1 事件类型

| 事件 | 说明 |
|------|------|
| `trace_start` | Trace 开始（userId/sessionId/input） |
| `user_input` | 用户输入 |
| `span_start` / `span_end` | Span 开始/结束（type/name/input/output/status） |
| `llm` | LLM 调用（model/temp/tokens/cost/mode/outcome/tools） |
| `tool` | 工具调用（toolName/input/output/exitCode/status） |
| `anomaly` | 异常（category/level/detail） |
| `log` | 通用日志消息 |
| `skill_*` / `skill_event` | Skill 生命周期事件 |
| `trace_end` | Trace 结束（status/finalOutput） |

## 4. 生命周期

```
AgentTraceInit(logDir):   打开 JSONL 文件（文件名含时间戳）
AgentTraceClose():        刷新并关闭文件
AgentTraceGetFilename():  返回当前文件名
```

`InitLogWithDir` 会委托 `AgentTraceInit`（见 observability_log.md），因此日志与追踪共用同一套文件基座。

## 5. Trace 与 Span 操作

### 5.1 Trace 开始/结束

```
AgentTraceStart(scope, userId, sessionId, input):
    生成 traceId（随机/时间戳）
    生成 rootSpanId
    scope.startTimeMs = now()
    写 trace_start 事件（含 userId/sessionId/input）

AgentTraceFinish(scope, status, finalOutput):
    汇总 token / 调用次数
    写 trace_end 事件（status/finalOutput）
```

### 5.2 Span 开始/结束

```
AgentTraceStartSpan(scope, span, parentSpanId, type, name, inputJson, metadataJson):
    生成 spanId
    span.traceId = scope.traceId
    span.parentSpanId = parentSpanId（NULL 则用 rootSpanId）
    span.startTimeMs = now()
    写 span_start 事件（含 inputJson/metadataJson）

AgentTraceEndSpan(span, status, outputJson):
    写 span_end 事件（含 status/outputJson、耗时）
```

Span 的类型由调用方传入（`llm` / `tool` / `router` 等），实现只负责记录。

## 6. 事件记录函数

### 6.1 AgentTraceLogLlm

```
AgentTraceLogLlm(scope, span, model, temperature, prompt, messages, response,
                 promptTokens, completionTokens, cost, mode, outcome, toolCallCount, toolNamesJson):
    累计 scope.totalPromptTokens / totalCompletionTokens / llmCallCount
    # 按 mode 决定 prompt/messages 记录策略
    if mode == FULL:
        记录完整 prompt + messages
    else: # INCREMENTAL
        # 若首轮未记录 prompt，记录 systemPromptHash 与完整 prompt
        # messages 只记录自 lastMessageCount 以来的增量，并更新 lastMessageCount
    写 llm 事件
    若 outcome=="error": 记录 anomaly
```

### 6.2 AgentTraceLogTool

```
AgentTraceLogTool(scope, span, toolName, inputJson, outputText, exitCode, status):
    更新 scope.toolCallCount
    写 tool 事件（含输入/输出/退出码/状态）
    输出超长时截断至 AGENT_TRACE_TOOL_OUTPUT_MAX(16KB)
    若 exitCode!=0: 记录 anomaly
```

### 6.3 AgentTraceLogAnomaly

```
AgentTraceLogAnomaly(scope, parentSpanId, category, level, detail):
    写 anomaly 事件（category / level→warn|error / detail）
```

### 6.4 AgentTraceLogSkill

```
AgentTraceLogSkill(scope, skillEvent):
    按 eventType 写 skill 事件；若 skillEvent.spanId 非空则关联该 span
    否则自动生成一个 skill spanId 并把 scope.skillSpanId 关联到该 skill 后续事件
```

### 6.5 AgentTraceLogMessage

```
AgentTraceLogMessage(level, component, message):
    写 log 事件（level/component/message，message 上限 2048）
```

## 7. 线程安全

所有事件写入通过**互斥锁**串行化，保证多线程（网络线程、Agent 线程）并发记录时不产生交错行。每个事件 `fprintf + fflush` 即时落盘。

## 8. 对外接口签名（汇总）

```c
int   AgentTraceInit(const char *logDir);
void  AgentTraceClose(void);
const char *AgentTraceGetFilename(void);
int   AgentTraceStart(AgentTraceScope *scope, const char *userId, const char *sessionId, const char *input);
void  AgentTraceFinish(AgentTraceScope *scope, const char *status, const char *finalOutput);
int   AgentTraceStartSpan(AgentTraceScope *scope, AgentTraceSpan *span, const char *parentSpanId,
                          const char *type, const char *name, const char *inputJson, const char *metadataJson);
void  AgentTraceEndSpan(AgentTraceSpan *span, const char *status, const char *outputJson);
void  AgentTraceLogLlm(...);
void  AgentTraceLogTool(...);
void  AgentTraceLogAnomaly(AgentTraceScope *scope, const char *parentSpanId,
                           const char *category, AgentTraceAnomalyLevel level, const char *detail);
void  AgentTraceLogSkill(AgentTraceScope *scope, const AgentTraceSkillEvent *skillEvent);
void  AgentTraceLogMessage(const char *level, const char *component, const char *message);
```

## 9. 复现要点（检查清单）

- [ ] Trace/Span/SkillEvent 字段完整，随事件写入 JSONL。
- [ ] span 之间通过 traceId/parentSpanId 关联成树。
- [ ] LLM 增量模式下 token 累计与 message 增量计算正确。
- [ ] 输出超长截断（工具 16KB、日志 2048）。
- [ ] 多线程记录线程安全，行不交错，即时落盘。

## 10. 相关文档

- `observability_log.md`：日志模块如何委托本模块。
- `kernel_agent_loop.md` / `kernel_skill_handler.md`：Span 与技能事件的上游调用。
