# kernel_skill_state：Skill 状态超集与挂起队列

## 1. 模块职责

`kernel/skill/skill_state_superset` 是技能运行状态的**单一真相源（single source of truth）**。它把原本分散在多个对象里的 skill 状态统一进一个 `SkillStateSuperset` 结构：

1. **基本标识**（SkillBasicInfo）：技能名、本次运行 ID、脚本目录。
2. **执行状态**（SkillExecState）：是否活跃、执行阶段、步骤数、LLM 调用数、时间戳、结果描述。
3. **元数据属性**（SkillMetaInfo）：副作用/可缓存/幂等/可恢复/单例/多轮/长生命周期等，从 SkillEntry 复制，运行期只读。
4. **追踪信息**（SkillTraceInfo）：Span ID、累计 token。
5. **挂起队列**（SkillSuspendQueue）与**执行历史存储**（ExecutionStateStore）：用于技能多轮对话的挂起/恢复机制。

设计目标：
- **消除手动同步**：不再需要双向同步函数，状态通过统一 API 更新，并内置日志。
- **自动日志**：每次状态变更经由统一 API 触发。

## 2. 关键数据结构

### 2.1 子集合

```c
/* 基本标识（激活时设置，运行期只读） */
typedef struct {
    char skillName[128];    /* skill 名称 */
    char skillRunId[64];    /* 运行 ID，如 "Hems#001" */
    char scriptsDir[256];   /* 脚本目录（相对于 skills 根目录） */
} SkillBasicInfo;

/* 执行阶段枚举 */
typedef enum {
    SKILL_EXEC_PHASE_INACTIVE = 0,  /* 未激活 */
    SKILL_EXEC_PHASE_RUNNING,       /* 运行中 */
    SKILL_EXEC_PHASE_WAITING_INPUT, /* 等待用户输入 */
    SKILL_EXEC_PHASE_RESUMING,      /* 恢复中 */
    SKILL_EXEC_PHASE_SUCCESS,       /* 成功（终态） */
    SKILL_EXEC_PHASE_FAILED,        /* 失败（终态） */
    SKILL_EXEC_PHASE_PARTIAL        /* 部分完成（终态） */
} SkillExecPhase;

/* 执行运行时状态 */
typedef struct {
    int  active;                 /* 是否有活跃 skill */
    SkillExecPhase phase;        /* 当前阶段 */
    int  stepCount;              /* 已执行步骤数 */
    int  llmCalls;               /* LLM 调用次数 */
    long long createdTimeMs;     /* 创建时间（毫秒） */
    long long lastActiveTimeMs;  /* 最后活跃时间（毫秒） */
    char lastOutcome[16];        /* 最终结果描述 */
} SkillExecState;

/* 元数据属性（从 SkillEntry 复制，运行期只读） */
typedef struct {
    int sideEffect; int cacheable; int idempotent;
    int resumable; int singleton; int multiTurn; int longRunning;
} SkillMetaInfo;

/* 追踪辅助信息 */
typedef struct {
    char skillSpanId[64];   /* 活跃 skill 的 Span ID */
    int totalPromptTokens;
    int totalCompletionTokens;
} SkillTraceInfo;
```

### 2.2 超集合本体

```c
typedef struct {
    SkillBasicInfo basic;   /* 激活时设置，运行期只读 */
    SkillExecState exec;    /* 运行时更新 */
    SkillMetaInfo  meta;    /* 运行期只读 */
    SkillTraceInfo trace;   /* agent_loop 更新 */
} SkillStateSuperset;
```

### 2.3 历史存储与挂起队列

```c
/* 历史条目（execStore 中保存每个 skill 实例的最终快照） */
typedef struct {
    SkillBasicInfo basic;
    SkillExecState exec;
} SkillHistoryEntry;

typedef struct {
    SkillHistoryEntry entries[8];  /* 最多 8 条 */
    int entryCount;
    unsigned long nextExecSeq;     /* 用于生成 runId 序号 */
} ExecutionStateStore;

/* 挂起条目（保存挂起时的完整状态） */
typedef struct {
    SkillBasicInfo basic;
    SkillExecState exec;
    SkillMetaInfo  meta;
} SkillSuspendEntry;

typedef struct {
    SkillSuspendEntry entries[4];  /* 最多 4 条 */
    int count;
} SkillSuspendQueue;
```

## 3. 生命周期 API

### 3.1 激活（SkillSupersetActivate）

```
SkillSupersetActivate(skillName, scriptsDir, meta):
    if 已有活跃 skill:
        先把当前活跃 skill 快照存入 execStore   # 避免丢失
    生成 runId：skillName + "#" + 三位序号（如 "Hems#001"）
    basic.skillName = skillName
    basic.scriptsDir = scriptsDir
    exec.active = true
    exec.phase = RUNNING
    exec.stepCount = 0
    exec.llmCalls = 0
    exec.createdTimeMs = now()
    exec.lastActiveTimeMs = now()
    meta = 传入 meta（复制），若 NULL 则清零
    return 0
```

要点：`nextExecSeq` 全局递增保证每次激活 runId 唯一。

### 3.2 结束（SkillSupersetFinish）

```
SkillSupersetFinish(finalPhase, outcome):
    if 无活跃 skill: return -1
    exec.phase = finalPhase            # SUCCESS/FAILED/PARTIAL
    if outcome: exec.lastOutcome = outcome
    把完整快照（basic + exec）追加到 execStore   # 归档
    重置超集合为 INACTIVE（active=false, 清零执行计数）
    return 0
```

### 3.3 重置与 SessionClose

- `SkillSupersetReset()`：直接重置为 INACTIVE（在 session open 时调用）。
- `SkillSupersetOnSessionClose()`：若仍有活跃 skill，先以 **PARTIAL**（outcome="session_closed"）结束并归档，再清理超集合。

### 3.4 执行状态更新

- `SkillSupersetUpdateExecPhase(newPhase, reason)`：更新阶段并记录状态变更日志。
- `SkillSupersetIncrementStep(&outStepIndex)`：`exec.stepCount++`，返回递增后的序号。
- `SkillSupersetIncrementLlmCalls()`：`exec.llmCalls++`。

### 3.5 查询

- `SkillSupersetGetActive()`：返回当前 active 超集合指针；无活跃返回 NULL（**只读**）。
- `SkillExecPhaseToString(phase)`：枚举转字符串，用于日志。

## 4. 挂起队列（Suspend Queue）

用于多轮可恢复技能：当 LLM 只返回纯文本（技能等待用户输入）时，把当前活跃技能压入队列；下次用户回复时按需恢复。

### 4.1 入队（SkillSuspendQueuePush）

```
SkillSuspendQueuePush(superset):
    # 把 superset 的 basic/exec/meta 复制为一条 SkillSuspendEntry
    追加到 entries[]
    count++
    # 队列满（>4）时，丢弃最旧条目
```

### 4.2 出队（SkillSuspendQueuePopByName）

```
SkillSuspendQueuePopByName(skillName, out):
    # 从后向前（最新优先）查找同名条目
    for i = count-1 downto 0:
        if entries[i].basic.skillName == skillName:
            *out = entries[i]
            删除该条目（后续前移）
            return 0
    return -1   # 未找到
```

设计要点：**后进先出优先恢复同名最新实例**。

### 4.3 清空 / 计数

- `SkillSuspendQueueClear()`：清空（Session Close 时调用）。
- `SkillSuspendQueueGetCount()`：返回条目数。

### 4.4 构建简单上下文（SkillSuspendQueueBuildSimpleContext）

```
SkillSuspendQueueBuildSimpleContext(out, outSize):
    # 输出格式（最新在前，供 Router 提示词使用）：
    #   - Hems (steps=2)
    #   - PLC_Monitor (steps=1)
    仅含信息，不含恢复指令
```

### 4.5 恢复（SkillSupersetReactivate）

```
SkillSupersetReactivate(entry):
    把 entry 的 basic/exec/meta 恢复到超集合
    exec.active = true
    exec.phase = RESUMING
    exec.lastActiveTimeMs = now()
    return 0
```

注意：恢复时**保留原来的 stepCount / llmCalls**，便于追踪累计进度。

## 5. 状态机流转图

```
        SkillSupersetActivate
                 │
                 ▼
   ┌──► RUNNING ──────────────┐
   │        │                 │ 工具结果 exit_code!=0
   │        │ 等待用户输入      ▼
   │        ▼               FAILED
   │   WAITING_INPUT ──恢复──► RESUMING ──► RUNNING
   │        │
   │        └─ 正常完成 ──► SUCCESS / PARTIAL
   │
   └── SkillSupersetFinish（归档到 execStore，重置）
```

## 6. 关键接口签名汇总

```c
const SkillStateSuperset *SkillSupersetGetActive(void);
int  SkillSupersetActivate(const char *skillName, const char *scriptsDir, const SkillMetaInfo *meta);
int  SkillSupersetFinish(SkillExecPhase finalPhase, const char *outcome);
void SkillSupersetReset(void);
void SkillSupersetOnSessionClose(void);
int  SkillSupersetUpdateExecPhase(SkillExecPhase newPhase, const char *reason);
int  SkillSupersetIncrementStep(int *outStepIndex);
int  SkillSupersetIncrementLlmCalls(void);
const char *SkillExecPhaseToString(SkillExecPhase phase);

int  SkillSuspendQueuePush(const SkillStateSuperset *superset);
int  SkillSuspendQueuePopByName(const char *skillName, SkillSuspendEntry *out);
void SkillSuspendQueueClear(void);
int  SkillSuspendQueueGetCount(void);
int  SkillSuspendQueueBuildSimpleContext(char *out, size_t outSize);
int  SkillSupersetReactivate(const SkillSuspendEntry *entry);
```

## 7. 复现要点（检查清单）

- [ ] 激活时若已有活跃 skill 会先归档当前快照，不丢状态。
- [ ] runId 单调递增且永不重复。
- [ ] `Finish` 把状态归档进 execStore 并正确重置。
- [ ] 挂起队列 PUSH/POP 行为正确，同名 POP 取最新。
- [ ] `Reactivate` 保留 stepCount/llmCalls，阶段置 RESUMING。
- [ ] Session Close 将残留 active skill 以 PARTIAL 结束。

## 8. 相关文档

- `kernel_skill_handler.md`：处理器的调用方（激活/挂起/结束）。
- `kernel_skill_router.md`：恢复路由如何使用挂起队列。
- `kernel_session.md`：超集合作为会话状态成员。
