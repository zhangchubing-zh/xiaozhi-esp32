# kernel_context：系统提示词上下文构建

## 1. 模块职责

`kernel/context/context_builder` 负责在每轮 LLM 调用前构建完整的 **system prompt**。内容包含三部分：

1. **基础系统指令**（LiteCrab 执行原则）——固定不变。
2. **可选技能列表**（selection metadata）——列出所有已加载技能，含名称、属性行、提示词，供模型决定是否及用哪个技能。
3. （工具 schema 不写进 system prompt，而是通过 OpenAI tools JSON 单独注册。）

## 2. 核心接口

```c
int ContextBuildSystemPromptEx(char *buf, size_t size, int includeSkills);
```

- `buf/size`：输出缓冲。
- `includeSkills`：为 1 时追加技能选择元数据段；为 0 时只输出基础指令。

## 3. 安全追加（SafeAppend）

内部用 `SafeAppend` 把格式化字符串安全追加到缓冲：

```
SafeAppend(buf, size, off, fmt, ...):
    if 参数无效或 off>=size: 置结尾 \0; return -1
    written = vsnprintf(buf+*off, size-*off, fmt, ...)
    if written<0: return -1
    if written >= size-*off: *off=size-1; 结尾\0; return -1   # 截断
    *off += written
    return 0
```

保证任何情况下缓冲都以 `\0` 结尾、不下溢。

## 4. 基础系统指令内容（固定段）

```
# LiteCrab

You are LiteCrab assistant. Be accurate and concise.

Runtime rules:
- Use the API-provided tools when work requires file access, search, shell execution, or skill loading.
- Do not claim work is done before executing the required tool calls and inspecting results.
- Treat tool outputs as intermediate state unless they fully answer the user request.
- Final answers should be brief and should not repeat large tool outputs.

Skill rules:
- Always call `skill_read` before executing any skill workflow, even if you recall it from conversation history.
- Use the skill list only to decide whether a skill applies.
- When a skill applies, call `skill_read` with the selected skill name as `skillPath`.
```

这段是英文，保持固定，便于复现时一致。

## 5. 技能选择元数据段（includeSkills=1）

```
if !includeSkills: 直接返回基础指令
skillCount = SkillGetEntries(skills, MAX_SKILLS)
if skillCount <= 0: return 0          # 无技能则不加段

追加：
"Available runtime skills (selection metadata only):\n"
"Choose a matching skill by name, pass that name as `skillPath` to `skill_read`, then follow the loaded skill instructions.\n"

for i in 0..skillCount-1:
    构建 propertiesLine（见下）
    若 propertiesLine 非空:
        追加  "## skill_<i>\nname: <name>\n<propertiesLine>\n<prompt>\n\n"
    否则:
        追加  "## skill_<i>\nname: <name>\n<prompt>\n\n"
    若 prompt 以换行结尾且缓冲末尾有多余换行，回退一个换行（避免空行堆积）
```

### 5.1 propertiesLine 属性行的生成

一组布尔属性按以下规则输出（**仅当属性为“值得提示”时输出**）：

| 条件 | 输出片段 |
|------|----------|
| `sideEffect` 为真 | ` side_effect=true,` |
| `!cacheable` | ` cacheable=false,` |
| `!idempotent` | ` idempotent=false,` |
| `resumable` 为真 | ` resumable=true,` |
| `singleton` 为真 | ` singleton=true,` |
| `multiTurn` 为真 | ` multi_turn=true,` |
| `longRunning` 为真 | ` long_running=true,` |

最后去掉末尾多余逗号。若所有属性均为「不提示」状态，`propertiesLine` 为空串，则输出不带属性行的格式。

## 6. 与 LLM 调用的衔接

`ContextBuildSystemPromptEx(systemPrompt, size, 1)` 在 `LlmIterRun` 的**每次迭代**开头调用（见 kernel_agent_loop.md），然后把 `systemPrompt` 作为 system 消息传给 `LlmChatToolsEx`。

## 7. 复现要点（检查清单）

- [ ] 基础指令段固定且完整。
- [ ] `includeSkills=0` 时不输出技能段。
- [ ] 技能属性行按上表规则生成，且末尾无多余逗号。
- [ ] prompt 末尾换行处理正确，不产生多余空行。
- [ ] 所有追加都经过 SafeAppend，缓冲安全截断。

## 8. 相关文档

- `kernel_skill_handler.md`：SkillEntry 与属性来源（skill_loader）。
- `kernel_agent_loop.md`：在每次迭代中调用本模块。
