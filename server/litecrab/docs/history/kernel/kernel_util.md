# kernel_util：通用工具（消息树 / JSON 提取 / 常量 / 字符串）

## 1. 模块职责

`kernel/util` 提供一组**纯函数、无全局状态**的基础工具，被 kernel 各子模块广泛复用：

- **message_tree**：对 cJSON 消息数组做**增量追加**（assistant / user / tool_calls + tool result）与序列化。
- **json_extract**：从 JSON 字符串提取字段；从文本提取标记整数。
- **kernel_constants**：kernel 层公共常量统一定义。
- **safe_string**：安全字符串拷贝（空指针保护 + 截断）。

设计原则：只操作传入的 cJSON 树，不做反复 Parse/Print 循环；无副作用；可跨模块复用。

## 2. kernel_constants

统一会话与工具常量，供 agent_loop / session_state / skill_handler / message_tree 等引用：

```c
/* 会话 */
#define AGENT_MAX_CONVERSATION_BYTES    (24 * 1024)  /* 单会话最大历史字节数 */
#define AGENT_MAX_SESSION_ID_LEN        64
#define AGENT_MAX_USER_ID_LEN           64
#define AGENT_MAX_CONVERSATION_MESSAGES 32

/* 工具执行 */
#define AGENT_TOOL_OUTPUT_MAX           (16 * 1024)  /* 单个工具输出缓冲区大小 */
```

## 3. safe_string

```c
static inline void SafeStrCopy(char *dst, size_t dstSize, const char *src)
{
    if (!src || !dst || dstSize == 0) return;
    snprintf(dst, dstSize, "%s", src);   /* 自动截断 + 保证 '\0' */
}
```

统一替换各模块中的局部复制，提供空指针保护与截断保护。

## 4. json_extract

### 4.1 JsonExtractStringField

```
JsonExtractStringField(input, fieldName, out, outSize):
    root = cJSON_Parse(input)
    if !root 或非对象: return 0
    item = cJSON_GetObjectItem(root, fieldName)
    if item 且是字符串: 拷贝到 out（截断 outSize-1）
    清理 root
    return 是否提取成功（1 成功 / 0 失败）
```

### 4.2 JsonExtractSkillPath

委托 `JsonExtractStringField(input, "skillPath", out, outSize)` 提取 `skill_read` 工具 input 里的技能路径。返回 1 成功 / 0 失败。这是技能处理与路由器恢复的核心解析依赖。

### 4.3 TextExtractMarkerInt

```
TextExtractMarkerInt(text, marker, outValue):
    在 text 中查找 marker 子串
    若找到：跳过后继空白，解析第一个整数值（如 "exit_code:" 后跟数字）
    *outValue = 解析值
    return 1   # 成功
    return 0   # 未找到标记或解析失败
```

用途：从工具输出文本提取 `exit_code:` 标记后的整数（见 skill_handler）。

## 5. message_tree

对 cJSON 数组做增量操作，避免整个历史反复重建。

### 5.1 MessageTreeAppendAssistant / MessageTreeAppendUser

```
MessageTreeAppendAssistant(arr, content):
    msg = object{ role:"assistant", content:<content> }
    arr.append(msg)

MessageTreeAppendUser(arr, content):
    msg = object{ role:"user", content:<content> }
    arr.append(msg)
```

### 5.2 MessageTreeAppendToolUseAndResult —— 核心

同时追加两条消息节点：**assistant（含 tool_calls）** + 每条调用对应的 **tool 结果**。

```
MessageTreeAppendToolUseAndResult(arr, resp, toolOutputs):
    # (1) assistant 消息
    assistantMsg = object{ role:"assistant" }
    toolCallsArray = []
    for i in 0..resp.callCount-1:
        call = object{
            "id": resp.calls[i].id,
            "type": "function",
            "function": {
                "name": resp.calls[i].name,
                "arguments": resp.calls[i].input    # 字符串形式的 JSON
            }
        }
        toolCallsArray.append(call)
    assistantMsg.tool_calls = toolCallsArray
    arr.append(assistantMsg)

    # (2) 每条 tool 结果消息
    for i in 0..resp.callCount-1:
        toolMsg = object{
            "role": "tool",
            "tool_call_id": resp.calls[i].id,
            "content": toolOutputs[i]        # 对应工具执行输出
        }
        arr.append(toolMsg)
    return 0
```

该函数产出的消息结构与 OpenAI 协议的 `tool_calls` / `tool` 角色兼容，是 request 与 history 双写的基础。

### 5.3 MessageTreePrintToBuffer

```
MessageTreePrintToBuffer(arr, buf, bufSize):
    json = cJSON_PrintUnformatted(arr)
    if !json: return -1
    if strlen(json) >= bufSize: free(json); return -1   # 缓冲区不足
    copy json -> buf
    free(json)
    return 0
```

## 6. 复现要点（检查清单）

- [ ] `MessageTreeAppendToolUseAndResult` 同时追加 assistant.tool_calls 与 role=tool 结果，且 `tool_call_id` 与 `id` 对应。
- [ ] JSON 序列化不溢出缓冲区，返回 -1 时调用方有兜底。
- [ ] `JsonExtractSkillPath` 正确提取 `skillPath` 字段。
- [ ] `TextExtractMarkerInt` 能从工具输出解析 `exit_code:` 整数。
- [ ] `SafeStrCopy` 对 NULL 与超长安全。

## 7. 相关文档

- `kernel_agent_loop.md`：消息双写流程调用 message_tree。
- `kernel_skill_handler.md` / `kernel_skill_router.md`：JsonExtractSkillPath 与 TextExtractMarkerInt 的使用。
