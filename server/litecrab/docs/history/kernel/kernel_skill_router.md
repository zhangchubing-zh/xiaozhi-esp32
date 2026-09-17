# kernel_skill_router：技能恢复路由（Skill Resume Router）

## 1. 模块职责

`kernel/skill/skill_router` 实现**技能恢复路由**：当会话中存在挂起的技能且用户再次输入时，判断是否需要恢复某个挂起技能。

关键设计：
- 复用**同一个 LLM provider**，但**替换 system prompt**，并把可用工具**限制为仅 `skill_recover`**。
- 路由调用是**独立的、非流式**的 LLM 调用，与主对话迭代分离。
- 决策结果是一个 `action + skillName`：动作 1=需要恢复，0=跳过；若需恢复则调用 `SkillRouterRecover`。

## 2. 关键数据结构

```c
typedef struct {
    int action;           /* 1=需要恢复，0=跳过 */
    char skillName[128];  /* 需恢复的技能名 */
} SkillRouterResult;
```

Router 专用 tools JSON 缓存在全局 `g_routerToolsJson[1024]`（仅含 skill_recover 工具，Init 时预构建）。

## 3. 初始化（SkillRouterInit）

```
SkillRouterInit(catalog):
    toolsArr = []
    for each tool in catalog.tools:
        if tool.name == "skill_recover":
            # 只构建 skill_recover 的 OpenAI function 描述
            #   name, description, parameters{type:object,
            #     properties:{skillPath:{type:string,description:...}},
            #     required:["skillPath"], additionalProperties:"false"}
            toolsArr.append(...)
            break
    g_routerToolsJson = cJSON_PrintUnformatted(toolsArr)
```

注意：这里的 `additionalProperties` 被设为字符串 `"false"`（实现如此），复现时应保持为 JSON 布尔值更规范。

## 4. 路由决策（SkillRouterRun）

### 4.1 流程

```
SkillRouterRun(historyJson, userContent, traceScope):
    # 1) 构建挂起技能简单列表
    suspendList  = SkillSuspendQueueBuildSimpleContext()

    # 2) 提取最近一次 assistant 纯文本（供 LLM 参考上一轮上下文）
    recentText   = ExtractRecentAssistantText(historyJson)  # 从历史末尾向前找

    # 3) 构建 Router 专用 system prompt（见下）
    systemPrompt = ...

    # 4) 构建单条 user 消息：
    #    "[上一轮助手回复]\n<recentText>\n\n[用户输入]\n<userContent>"

    # 5) 起一个 router Span（trace）
    AgentTraceStartSpan(scope, &routerSpan, rootSpanId,
                        "router", "skill_resume_router", inputJSON)

    # 6) 调用 LLM（非流式 stream=0，仅 skill_recover 工具）
    rc = LlmChatToolsEx(systemPrompt, messagesJson, g_routerToolsJson, &resp, 0)
    if rc != 0: 记录 error span; return {action:0}

    # 7) 解析结果
    result = {action:0}
    if resp.toolUse && callCount>0:
        for each call:
            if call.name == "skill_recover":
                if JsonExtractSkillPath(call.input, &name):
                    result.action = 1
                    result.skillName = name
                break

    # 8) 记录决策结果 span（ok/skip）
    AgentTraceEndSpan(routerSpan, action? "ok":"skip", outputJSON)
    return result
```

### 4.2 Router 专用 System Prompt（示意）

```
# Skill Resume Router

你是一个技能恢复路由器。根据对话历史和用户输入，判断是否需要恢复某个挂起的技能。

## 挂起的技能
<suspendList>        # 如 "- Hems (steps=2)"

## 规则
- 如果用户输入是对某个挂起技能的回应（提供选项/参数/确认），调用 skill_recover 恢复该技能。
- 如果用户输入与所有挂起技能无关，直接回复 "NONE"，不要调用任何工具。
- 如果不确定，优先恢复列表中第一个技能。
- 只能调用 skill_recover 工具，不能调用其他工具。
```

### 4.3 提取最近 assistant 文本（ExtractRecentAssistantText）

从 `historyJson` 数组**末尾向前**查找第一条 `role==assistant` 且 `content` 非空的消息，取 `content`（截断到 outSize）。找不到返回空串。

## 5. 恢复执行（SkillRouterRecover）

```
SkillRouterRecover(skillName, traceScope, sessionId, requestId):
    rc = SkillSuspendQueuePopByName(skillName, &entry)   # 从队列弹出
    if rc != 0: log "未在挂起队列中找到"; return -1

    SkillSupersetReactivate(&entry)                      # 恢复超集合，阶段=RESUMING

    # 记录 skill_event 恢复事件（phase="resuming"）
    AgentTraceLogSkill(...)

    return 0
```

## 6. 调用时机

恢复路由在主循环处理 CHAT 消息时、**进入工具迭代之前**判断是否需要运行。典型触发条件：消息历史 / 本轮输入与某个挂起技能相关（可由每次处理 CHAT 统一执行 RouterRun，或按上下文判断）。

主循环在收到路由结果后：

```
if SkillRouterResult.action == 1:
    SkillRouterRecover(result.skillName, ...)   # 恢复挂起技能
    # 后续工具迭代会看到 RESUMING 状态的技能，继续执行其流程
```

## 7. 复现要点（检查清单）

- [ ] Router 只暴露 skill_recover 一个工具。
- [ ] 决策使用非流式独立 LLM 调用。
- [ ] 挂起列表为空时 prompt 中挂起区为空，路由通常返回 action=0。
- [ ] 能从 `skill_recover` 调用的 `skillPath` JSON 正确提取技能名。
- [ ] 恢复时从队列弹出同名最新条目并置阶段 RESUMING。
- [ ] Router 调用与主对话共用同一 LLM 配置（provider/model）。

## 8. 相关文档

- `kernel_skill_state.md`：挂起队列与 Reactivate 实现。
- `kernel_skill_handler.md`：处理器与路由的协作边界。
- `kernel_llm.md`：LlmChatToolsEx 非流式调用。
- `kernel_agent_loop.md`：路由在 CHAT 处理中的调用位置。
