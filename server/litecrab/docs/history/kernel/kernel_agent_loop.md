# kernel_agent_loop：Agent 主循环与消息处理

## 1. 模块职责

`kernel/agent_loop` 是 Agent 的**主控循环**，负责把「会话状态、LLM、技能、工具执行器」串成完整的对话处理流程。它：

1. 在 `AgentLoopInit` 中完成全局装配：初始化会话状态、运行时（runtime）、注册内置工具、启动 Agent 线程、填充工具目录、初始化工具执行器与技能路由器。
2. 通过 `AgentLoopStart` 创建一个独立 `AgentThread` 线程，持续从入站消息总线取出消息。
3. 在 `AgentThread` 主循环中按消息类型（SESSION_OPEN / SESSION_CLOSE / CHAT）分派处理。
4. 对 CHAT 消息调用 `LlmIterRunRequestRound`，执行「最多 MAX_TOOL_ITER 轮」的 LLM-工具迭代。

## 2. 关键数据结构

### 2.1 全局状态与上下文

```c
/* 全局工具 JSON（OpenAI tools 数组），一次渲染供整个进程复用 */
static char g_toolsJson[65536];

/* 单轮对话的上下文缓冲（均为栈上或成员缓冲，避免频繁分配） */
typedef struct {
    char sessionHistoryJson[AGENT_MAX_CONVERSATION_BYTES]; /* 会话历史 JSON */
    char requestMessagesJson[AGENT_MAX_CONVERSATION_BYTES]; /* 发给 LLM 的 messages 数组 */
    char historyBuildJson[AGENT_MAX_CONVERSATION_BYTES];    /* 追加历史用缓冲 */
    char toolOutputs[8][AGENT_TOOL_OUTPUT_MAX];             /* 8 个工具调用各自的输出 */
    char sessionId[AGENT_MAX_SESSION_ID_LEN];
    char userId[AGENT_MAX_USER_ID_LEN];
} AgentRoundCtx;
```

说明：`toolOutputs[8][16KB]` 的「8」对应最大工具调用/迭代数上限，每轮迭代最多执行 8 个工具调用，其输出存入对应槽位，再整体拼入消息历史。

### 2.2 全局对象（在 AgentLoopInit 中创建）

- `g_agentRuntime`：`CrabRuntime` 实例（runtime 模块核心）。
- 单例会话状态 `AgentSessionState`（见 kernel_session 文档）。
- 技能状态超集、技能处理器、技能路由器（见 kernel_skill 文档）。
- 工具执行器 `ToolExecutor`（见本文档第 5 节）。

## 3. 初始化流程（AgentLoopInit）

```
AgentLoopInit():
    AgentSessionStateInit()              # 会话状态单例初始化
    CrabRuntimeInit(&g_agentRuntime, NULL)
    CrabRuntimeRegisterBuiltinTools(&g_agentRuntime)   # 注册 read/shell 等
    ToolExecutorInit(&g_toolExecutor)
    SkillRouterInit()                    # 仅含 skill_recover 的路由器
    SkillLoadAll()                       # 从 skills/ 扫描加载技能
    # 将工具目录渲染为 OpenAI tools JSON，缓存到 g_toolsJson
    return ok
```

`AgentLoopStart()`：

```
AgentLoopStart():
    create_thread(AgentThread)
```

`AgentThread()` 主循环：

```
while (true):
    msg = pop inbound message bus (阻塞取出)
    switch msg.type:
        SESSION_OPEN:  AgentSessionStateOpen()
        SESSION_CLOSE: AgentSessionStateClose()
        CHAT:          HandleChatMessage(msg)
    free(msg.content)
```

## 4. 消息处理主循环（AgentThread / HandleChatMessage）

### 4.1 SESSION_OPEN 与 SESSION_CLOSE

- **SESSION_OPEN**：打开会话状态，绑定用户与会话 ID，恢复该会话此前保存的历史（若有）。
- **SESSION_CLOSE**：保存会话状态到磁盘，标记会话关闭，释放资源。

### 4.2 CHAT 消息（核心）

```
HandleChatMessage(msg):
    AgentSessionStateBindUser(msg.userId)      # 绑定当前用户
    AgentSessionStateOpen()                    # 打开会话
    sessionHistoryJson = AgentSessionStateLoad()  # 加载已有历史（JSON 数组字符串）

    # --- 准备缓冲与调 LLM 迭代 ---
    finalText = LlmIterRunRequestRound(
                    toolsJson,                   # 全局 g_toolsJson
                    sessionId, requestId,
                    userContent,
                    sessionHistoryJson, historySize,  # in-out：最终被更新为最新历史
                    requestMessagesJson, requestMsgsSize,
                    historyBuildJson, historyBuildSize,
                    toolOutputs,                 # char[8][16KB]
                    traceScope)

    AgentSessionStateSave(sessionHistoryJson)   # 持久化最新历史
    发送 finalText 回复（经 outbound 总线 push）
    free(finalText)
```

`LlmIterRunRequestRound` 负责把「用户新消息 + 已有历史」拼接后启动工具迭代（详见本文第 6 节与 llm_iteration）。

## 5. 工具执行器（ToolExecutor）

工具执行器把「LLM 返回的 tool_calls」翻译为 runtime 层的类型化 `CrabToolCall` 并执行。

```c
typedef struct {
    /* 按工具调用的索引保存执行后的响应（复用到 ctx.toolOutputs 槽位） */
} ToolExecutorState;

/* 关键函数 */
void ToolExecutorInit(ToolExecutorState *state);
int  ToolExecutorExecute(ToolExecutorState *state,
                         CrabRuntime *runtime,
                         const AgentRoundCtx *ctx,
                         int callIndex);   /* 执行单个工具调用 */
```

`ToolExecutorExecute` 流程：

1. 用 `LlmToolAdapterParseCall` 把第 `callIndex` 个工具调用从 JSON 参数解析为 `CrabToolCall`。
2. **shell 脚本目录自动重定向**：若工具名为 `shell` 且脚本以相对路径（如 `python x.py`）出现，且当前技能有 `scriptsDir`，则将工作路径重定向到 `../skills/<skillName>/scripts`，使技能脚本可直接运行。
3. 起一个追踪 Span（`AgentTraceStartSpan`）标记工具执行。
4. 调用 `CrabRuntimeCallTool(runtime, &call, &response)` 执行。
5. 记录 Agent 追踪 `LogTool` 事件。
6. 调用 `SkillHandlerOnToolResult`，让技能处理器检查退出码、步数等。
7. 把 `response.content` 写入 `ctx.toolOutputs[callIndex]`（受 `AGENT_TOOL_OUTPUT_MAX` 上限约束）。
8. 结束 Span。

## 6. 工具迭代主流程（LlmIterRunRequestRound / LlmIterRun）

`llm_iteration.c` 用两个函数实现迭代。关键签名：

```c
/* 返回: 0=继续(有工具调用), 1=结束(finalText 已设置), -1=错误 */
int LlmIterRun(int iteration, const char *toolsJson,
               const char *sessionId, const char *requestId,
               char *systemPrompt, size_t systemPromptSize,
               char *requestMessagesJson, size_t requestMsgsSize,
               char *historyBuildJson, size_t historyBuildSize,
               char (*toolOutputs)[AGENT_TOOL_OUTPUT_MAX],
               AgentTraceScope *traceScope, char **finalText);

/* 返回: finalText 字符串（调用方 free），NULL 表示无有效回复 */
char *LlmIterRunRequestRound(const char *toolsJson,
                             const char *sessionId, const char *requestId,
                             const char *userContent,
                             char *sessionHistoryJson, size_t historySize,
                             char *requestMessagesJson, size_t requestMsgsSize,
                             char *historyBuildJson, size_t historyBuildSize,
                             char (*toolOutputs)[AGENT_TOOL_OUTPUT_MAX],
                             AgentTraceScope *traceScope);
```

注意：此处没有 `AgentRoundCtx` 结构，而是直接传递多个缓冲。`MAX_TOOL_ITER = 8`；请求消息缓冲 `AGENT_REQUEST_JSON_MAX = AGENT_MAX_CONVERSATION_BYTES + 32KB`。

### 6.1 LlmIterRunRequestRound 流程

```
LlmIterRunRequestRound(...):
    historyArr = cJSON_Parse(sessionHistoryJson)       # 历史树
    requestArr = cJSON_Duplicate(historyArr, 1)        # 深拷贝为请求树
    MessageTreeAppendUser(requestArr, userContent)     # 追加当前用户消息
    MessageTreePrintToBuffer(requestArr, requestMessagesJson,...)  # 序列化请求消息
    MessageTreePrintToBuffer(historyArr, historyBuildJson,...)     # 历史构建缓冲初值

    iteration = 0; finalText = NULL
    while iteration < MAX_TOOL_ITER && finalText == NULL:
        iterRet = LlmIterRun(iteration, toolsJson, sessionId, requestId,
                             systemPrompt, ..., requestMessagesJson, ...,
                             historyBuildJson, ..., toolOutputs, traceScope, &finalText)
        if iterRet < 0: break          # 错误
        if iterRet > 0: break          # 已得最终文本
        # iterRet==0: 重启 requestArr/historyArr 反映最新缓冲，iteration++
        requestArr = cJSON_Parse(requestMessagesJson)
        historyArr = cJSON_Parse(historyBuildJson)
        iteration++

    if finalText == NULL && iteration >= MAX_TOOL_ITER:
        finalText = "Reached maximum tool iterations before the agent could finish."

    # 兜底：若循环退出时技能仍 active，则挂起
    if 技能 active: SkillSuspendQueuePush + SkillHandlerSuspend

    # 把最终 assistant 文本并入历史树
    if finalText: MessageTreeAppendAssistant(historyArr, finalText)
    MessageTreePrintToBuffer(historyArr, historyBuildJson, ...)
    MessageTreePrintToBuffer(historyArr, sessionHistoryJson, ...)   # 更新长期历史
    cJSON 清理两树
    return finalText
```

### 6.2 LlmIterRun 单次迭代流程

```
LlmIterRun(iteration, toolsJson, sessionId, requestId,
           systemPrompt, ..., requestMessagesJson, ..., historyBuildJson, ...,
           toolOutputs, traceScope, &finalText):
    # 1) 构建系统提示词
    ContextBuildSystemPromptEx(systemPrompt, size, 1)   # 失败 -> finalText=错误, return -1

    # 2) 调用 LLM
    LlmGetConfig(&config)
    AgentTraceStartSpan(..., "llm", "llm_chat_tools", llmMetadata)
    if 技能 active: SkillSupersetIncrementLlmCalls()
    rc = LlmChatToolsEx(systemPrompt, requestMessagesJson, toolsJson, &resp, config.stream)

    if rc != 0:
        # 错误：区分 -206 超长 / 其他；记录 LogLlm(error) + 结束 Span
        finalText = strdup(错误文本)
        return -1

    AgentTraceLogLlm(...)  # 记录满/增量模式
    AgentTraceEndSpan(llmSpan, "ok")

    # 3) 无工具调用 -> 纯文本，挂起技能，结束
    if !resp.toolUse:
        finalText = SkillHandlerExtractFinalText(resp.text, resp.textLen)
        if 技能 active: SkillSuspendQueuePush + SkillHandlerSuspend
        return 1

    # 4) 技能开始
    SkillHandlerOnSkillStart(traceScope, &resp, sessionId, requestId)

    # 5) 执行全部工具调用（结果写入 toolOutputs）
    SkillHandlerTurnState skillTurnState = {}
    ToolExecutorBuildResults(traceScope, &resp, toolOutputs, sessionId, requestId, &skillTurnState)

    # 6) 双写：tool_use/tool 结果追加到请求树与历史树
    reqArr  = cJSON_Parse(requestMessagesJson)
    histArr = cJSON_Parse(historyBuildJson)   # 若无效则新建数组
    MessageTreeAppendToolUseAndResult(reqArr, &resp, toolOutputs)
    MessageTreePrintToBuffer(reqArr, requestMessagesJson, ...)
    MessageTreeAppendToolUseAndResult(histArr, &resp, toolOutputs)
    MessageTreePrintToBuffer(histArr, historyBuildJson, ...)
    cJSON 清理
    return 0   # 继续下一轮
```

## 7. 关键算法与边界

## 7. 关键算法与边界

### 7.1 缓冲避免频繁分配
整个 AgentRoundCtx 在栈上（或调用者持有），工具输出复用固定槽位数组，避免每轮迭代重复 malloc/free 大块内存。这在大消息历史场景下有明显收益。

### 7.2 历史双写
每次工具迭代后，`tool_calls` 与 `tool` 结果消息**同时**追加到：
- `requestMessagesJson`（本轮发给 LLM 的完整上下文，含用户新消息）；
- `sessionHistoryJson`（会长久保存的会话历史，不含本轮顶层用户消息的重复注入问题由上层保证）。

这样既保证 LLM 下一轮能看到完整的工具执行结果，又保证最终整个对话被持久化。

### 7.3 迭代上限
`MAX_TOOL_ITER`（内核层默认 8）与 Hub 层的 `LITE_AGENT_MAX_TOOL_ITER`（4）不同，主循环以 LLM 迭代为实际轮数控制。达到上限后停止，避免模型无限循环调用工具。

### 7.4 错误处理
- LLM 调用失败（网络、超时、Provider 错误等）：返回 `LLM_ITER_ERROR`，主循环将此轮作异常处理（记录追踪异常事件，必要时返回错误文本）。
- 工具执行失败：不中断整体循环，错误信息会写入 tool 结果消息并由后续 LLM 轮次感知。

## 8. 对外关键接口签名

```c
/* agent_loop.h */
int  AgentLoopInit(void);
int  AgentLoopStart(void);
void AgentLoopStop(void);

/* 获取工具目录（供 tool 渲染与注册） */
int  AgentLoopGetToolCatalog(CrabToolCatalogView *catalog);

/* 提供给 LLM 工具适配器使用的全局渲染入口 */
const char *AgentLoopGetToolsJson(void);
```

## 9. 复现要点（检查清单）

- [ ] 能注册内置工具并渲染出合法的 OpenAI tools JSON（`g_toolsJson`）。
- [ ] 独立 AgentThread 能从入站总线取消息并按类型分派。
- [ ] CHAT 处理能正确走「历史加载 -> 用户消息追加 -> 工具迭代 -> 历史保存」。
- [ ] 遇到工具调用会进入迭代并在退出码标记后回到下一轮 LLM。
- [ ] 无工具调用时能直接返回最终文本并结束本请求。
- [ ] 会话历史与请求上下文保持一致性（双写）。

## 10. 相关文档

- `kernel_session.md`：会话状态机与历史裁剪。
- `kernel_llm.md`：LLM 客户端与流式解析。
- `kernel_skill.md`：技能处理器/路由器协作。
- `kernel_context.md`：系统提示词构建。
- `kernel_util.md`：消息树与常量。
