# kernel_skill_handler：技能处理器与技能加载器

## 概览

本文档覆盖 `kernel/skill` 下两个紧密相关的模块：

- **skill_handler.c**：技能生命周期事件处理（启动 / 工具结果 / 挂起 / 结束），是 agent_loop 中所有技能编排逻辑的载体。
- **skill_loader.c/.h**：扫描 `../skills` 目录，解析每个 SKILL.md 的 front-matter，缓存技能条目供查询。

## 一、Skill 处理器（SkillHandler）

### 1. 模块职责

处理器把「LLM 返回的工具调用」映射为技能的启动/挂起/结束动作，并负责技能步进日志与最终文本处理。它以 skill_state_superset（见 kernel_skill_state.md）为底层。

### 2. 单轮状态跟踪（SkillHandlerTurnState）

```c
typedef struct {
    int relatedToolSeen;  /* 本轮是否已遇到 skill 相关工具（shell） */
    int hasLastExitCode;  /* 是否有最新退出码 */
    int lastExitCode;     /* 最新退出码 */
} SkillHandlerTurnState;
```

在每轮 `LlmIterRun` 中作为局部变量传入 `ToolExecutorBuildResults`，用于汇总本轮 shell 工具的执行结果。

### 3. 核心接口与算法

#### 3.1 SkillHandlerOnSkillStart —— 技能启动

遍历 LLM 响应中的所有工具调用，仅关心 `skill_read` 与 `skill_recover`：

```
SkillHandlerOnSkillStart(scope, resp, sessionId, requestId):
    for each call in resp.calls:
        if call.name 不是 skill_read/skill_recover: continue
        if !JsonExtractSkillPath(call.input, &skillName): continue   # 取 skillPath

        # (a) 优先尝试从挂起队列恢复同名 skill
        if SkillSuspendQueuePopByName(skillName, &entry) == 0:
            SkillSupersetReactivate(&entry)
            LogSkillStart(...)      # 记录恢复的 skill_start
            continue                # 恢复完成，不新建实例

        # (b) skill_recover 只恢复挂起技能，不创建新实例
        if isRecoverCall: 
            log "无挂起技能可恢复"; continue

        # (c) 若当前已有活跃 skill 且不同名，先挂起旧的
        if 当前活跃且 active:
            if 同名: continue      # 同名已激活，跳过
            SkillSuspendQueuePush(当前活跃)
            SkillHandlerSuspend(...)

        # (d) 从已加载技能列表查找 scriptsDir 与 meta
        scriptsDir = 查找到的 scriptsDir
        meta = 从 SkillGetEntries 复制 7 个布尔属性

        # (e) 激活新 skill
        SkillSupersetActivate(skillName, scriptsDir, &meta)
        LogSkillStart(...)
```

**优先级**：队列恢复 > 不新建（skill_recover）> 正常激活。

#### 3.2 SkillHandlerOnToolResult —— 工具结果处理

```
SkillHandlerOnToolResult(scope, call, toolOutput, sessionId, requestId, turnState):
    if 无活跃 skill: return

    # skill_read / skill_recover：只检查退出码，不记录 step
    if call.name 是 skill_read 或 skill_recover:
        if TextExtractMarkerInt(toolOutput, "exit_code:", &exitCode) && exitCode != 0:
            SkillHandlerFinish(FAILED)    # 读取失败 -> 技能失败
        return

    # 只处理 shell 工具
    if call.name != "shell": return

    if !TextExtractMarkerInt(toolOutput, "exit_code:", &exitCode):
        exitCode = -1                     # 无法提取视为失败
    SkillSupersetIncrementStep(&stepIndex)
    LogSkillStep(...)                     # 记录 skill_step 事件
    turnState.relatedToolSeen = 1
    turnState.hasLastExitCode = 1
    turnState.lastExitCode = exitCode
```

要点：`exit_code:` 从工具输出文本中提取（工具响应包含 `exit_code: N` 标记行）。

#### 3.3 SkillHandlerSuspend —— 挂起

```
SkillHandlerSuspend(scope, sessionId, requestId):
    if 无活跃 skill: return
    snapshot = *superset                 # 保存快照用于日志
    SkillSupersetFinish(PARTIAL, "suspended")   # 归档 + 重置超集合
    LogSkillEnd(snapshot, PARTIAL)               # 记录挂起日志/trace
```

**注意**：调用方需先执行 `SkillSuspendQueuePush` 再调用本函数（队列入队与超集合重置分离）。

#### 3.4 SkillHandlerFinish —— 结束

```
SkillHandlerFinish(scope, sessionId, requestId, finalPhase):
    if 无活跃 skill: return
    snapshot = *superset
    outcomeStr = 按 finalPhase 映射: SUCCESS->"ok", FAILED->"fail", PARTIAL->"partial"
    SkillSupersetFinish(finalPhase, outcomeStr)   # 归档 + 重置
    LogSkillEnd(snapshot, finalPhase)
```

#### 3.5 SkillHandlerExtractFinalText —— 提取最终文本

```
SkillHandlerExtractFinalText(text, textLen):
    copy = strdup(text)
    去除末尾空白（\n \r \t 空格）
    return copy          # 调用方负责 free
```

#### 3.6 SkillHandlerBuildToolNamesJson —— 工具名 JSON

把 `resp.calls[].name` 汇总为 `["read","shell",...]` 的 JSON 数组字符串，用于 trace 元数据。

### 4. 日志与 Trace 事件

处理器通过 `LogSkillStart / LogSkillStep / LogSkillEnd` 向 observability 写入两种记录：

- **LogPrint**（文本日志）：`[agent] skill_start ...`、`[agent] skill_step ...`、`[agent] skill_end ...`。
- **AgentTraceLogSkill**（结构化事件）：`skill_start` / `skill_step` / `skill_end` / `skill_event` 四种 eventType。

这些事件携带 skillRunId / sessionId / requestId / name / phase / step / exitCode / 计数等字段。

## 二、Skill 加载器（SkillLoader）

### 1. 模块职责

`SkillLoadAll` 扫描 skills 根目录（默认 `../skills`），解析每个子目录下的 `SKILL.md`，提取技能名称、提示词与元数据，并缓存到全局条目数组。

### 2. SkillEntry 结构

```c
#define MAX_SKILLS 32
#define SKILL_NAME_SIZE 128
#define SKILL_PROMPT_SIZE 2048
#define SKILL_PATH_SIZE 512
#define SKILL_SCRIPTS_DIR_SIZE 256

typedef struct {
    char name[128];       /* 技能名 */
    char prompt[2048];    /* 提示词（SKILL.md 解析） */
    char path[512];       /* SKILL.md 完整路径 */
    char scriptsDir[256]; /* 脚本目录（相对 skills 根目录） */
    int sideEffect, cacheable, idempotent, resumable;
    int singleton, multiTurn, longRunning;
} SkillEntry;
```

### 3. 解析算法（ParseSkillFile）

解析规则是针对 `SKILL.md` 的 **front-matter** 提取：

```
ParseSkillFile(filePath, &entry):
    content = read(filePath)

    # 定位第一、二个独占一行的 "---" 分隔符（YAML front-matter）
    firstSep = 找第一个独占行 "---"
    secondSep = 找第二个独占行 "---"
    frontMatter = 介于 firstSep 与 secondSep 之间的文本
    body = secondSep 之后的内容（不使用，加载器主要用 front-matter）

    # 名称：通常在 front-matter 中 "name: Xxx"
    name = 提取 front-matter 的 name 字段

    # 布尔元数据：遍历 front-matter 行，识别
    #   side-effect / cacheable / idempotent / resumable /
    #   singleton / multi-turn / long-running  等键
    #   值为 true/false/1/0
    scriptsDir 自动推断 = "<技能名>/scripts"
    提示词 prompt = front-matter 其余说明文本

    若 front-matter 含 HTML 实体（如 &#x20;），执行 DecodeHtmlEntities 解码
```

要点：
- **只解析 front-matter**：名称与元数据从两对 `---` 之间提取。
- 特殊字符：解析后需把 `&#x20;` 等 HTML 实体的空格还原为真实字符（容错文案里的转义空格）。
- `scriptsDir` 自动设为 `{name}/scripts`，供工具执行时定位脚本。

### 4. 目录扫描（SkillLoadAll）

```
SkillLoadAll():
    SKILLS_ROOT = "../skills"
    遍历 SKILLS_ROOT 下每个子目录 dir:
        mdPath = dir + "/SKILL.md"
        if 存在:
            ParseSkillFile(mdPath, &entry)
            追加到全局条目数组 g_skillEntries
    return 0
```

**平台兼容**：Windows 与 Linux 分别使用不同目录遍历 API（Windows 用 FindFirstFile/FindNextFile，Linux 用 opendir/readdir）。

### 5. 对外接口

```c
int SkillLoadAll(void);
int SkillGetEntries(SkillEntry *outEntries, int maxEntries);  /* 返回条目数 */
```

## 三、复现要点（检查清单）

### 处理器
- [ ] OnSkillStart 优先从挂起队列恢复，其次才激活新实例。
- [ ] skill_recover 不创建新实例，只恢复。
- [ ] skill_read/skill_recover 负责加载状态，其余工具都作为技能步骤记录；skill_read 失败使技能 FAILED。
- [ ] 挂起前调用方先 Push 队列，然后 Suspend 完成归档+重置。
- [ ] 用 `exit_code:` 标记从工具输出提取退出码。

### 加载器
- [ ] 从两个 `---` 之间正确解析 name 与布尔元数据。
- [ ] `scriptsDir` 自动推断为 `{name}/scripts`。
- [ ] HTML 实体（`&#x20;`）解码正确。
- [ ] Win/Linux 目录遍历都可扫描出 `SKILL.md`。

## 四、相关文档

- `kernel_skill_state.md`：底层状态超集与挂起队列。
- `kernel_skill_router.md`：恢复路由与处理器协作。
- `kernel_agent_loop.md`：`LlmIterRun` 中调用 OnSkillStart / OnToolResult。
- `kernel_llm.md`：LlmResponse / LlmToolCall 结构。
