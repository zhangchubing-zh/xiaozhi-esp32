# observability_log：日志模块

## 1. 模块职责

`observability/log` 提供基于文件的**简易日志**接口，并把日志统一接入 Agent 追踪（AgentTrace）的结构化 JSONL 事件，使普通文本日志与链路追踪共用一套记录基座。

## 2. 全局状态

```c
extern FILE *g_logFp;            /* 日志文件指针 */
extern char g_logFilename[256];  /* 日志文件名 */
```

## 3. 生命周期

```c
int InitLog(void);               /* 使用默认目录初始化 */
int InitLogWithDir(const char *logDir);   /* 使用指定目录初始化 */
void CloseLog(void);             /* 关闭日志文件 */
```

- `InitLogWithDir(logDir)` 负责打开日志文件，并**委托 `AgentTraceInit(logDir)`** 初始化追踪模块（二者共用同一目录）。
- `CloseLog` 关闭文件指针并置空。

## 4. 日志写入（LogPrint）—— 核心

```c
void LogPrint(const char *fmt, ...);
```

流程：

```
LogPrint(fmt, ...):
    if !g_logFp: 丢弃或回退到 stderr
    vsnprintf(buf, fmt, ...)                # 格式化
    去掉末尾多余换行（裁剪尾部 \r\n）

    # 解析组件与推断级别
    component = 提取日志行中的 [section] 标记（如 "[agent]"）
    level = 通过关键字推断（如发现 "ERROR"/"failed" -> error；
             "WARNING"/"WARN" -> warn；"success" -> info；默认 info）

    # 交给 AgentTrace 记录为结构化 log 事件
    AgentTraceLogMessage(level, component, message)
```

要点：
- **裁剪尾部换行**：避免 JSONL 事件内混入换行导致每行不再是合法 JSON。
- **组件提取**：从日志前缀 `[xxx]` 提取组件名。
- **级别推断**：基于子串关键词判断 level，兼容各类日志写法。

## 5. 使用示例

```c
LogPrint("[agent] llm iteration=%d stream=%d\n", iteration + 1, stream);
LogPrint("[skill_router] result: action=%d skillName='%s'\n", ...);
```

这些调用最终都会变成 `agent_trace` JSONL 文件的 `{ "type":"log", "level":"info", "component":"agent", "message":"..." }` 记录。

## 6. 复现要点（检查清单）

- [ ] InitLogWithDir 同时初始化日志与追踪，共享目录。
- [ ] LogPrint 去除尾部换行，避免破坏 JSONL 行结构。
- [ ] 组件 `[xxx]` 提取正确。
- [ ] 级别按关键词推断合理。
- [ ] g_logFp 为空时安全（不崩溃）。

## 7. 相关文档

- `observability_agent_trace.md`：AgentTraceLogMessage 记录格式。
- 各 kernel/runtime 文档：LogPrint 在模块日志中的使用。
