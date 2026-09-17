# kernel_session：会话状态管理

## 1. 模块职责

`kernel/agent_loop/session_state` 维护 Agent 的**单一活动会话状态**，是消息历史与用户绑定的权威来源。它：

1. 管理当前会话的 userId / sessionId 绑定。
2. 以 JSON 消息数组形式保存会话历史（`messagesJson`）。
3. 提供历史裁剪策略（固定条数 + 固定字节数双重上限），避免上下文无限膨胀。
4. 提供会话打开 / 绑定用户 / 加载 / 保存 / 关闭的生命周期接口。

设计上采用**静态单例**：同一时刻只处理一个会话，状态常驻内存。

## 2. 关键数据结构

```c
/* 会话状态超集（结构体定义，作为单例的成员） */
typedef struct {
    int inUse;                          /* 会话是否占用 */
    char userId[AGENT_MAX_USER_ID_LEN];       /* 当前用户 ID */
    char sessionId[AGENT_MAX_SESSION_ID_LEN]; /* 当前会话 ID */
    char messagesJson[AGENT_MAX_CONVERSATION_BYTES]; /* 会话消息历史 JSON 数组 */
    size_t messagesLen;                 /* 已用字节长度（<= 24KB） */

    /* 技能相关状态（详见 kernel_skill） */
    void *skillSuperset;    /* 技能状态超集实例 */
    void *suspendQueue;     /* 挂起队列 */
    void *execStore;        /* 执行历史存储 */
} AgentSessionState;
```

`messagesJson` 的语义：一个**数组**，元素为 OpenAI 消息对象：

```json
[
  {"role": "user",      "content": "用户的输入"},
  {"role": "assistant", "content": "助手回复"},
  {"role": "assistant", "content": null,
   "tool_calls": [{"id":"call_1","type":"function",
      "function":{"name":"read","arguments":"{\"path\":\"a.txt\"}"}}]},
  {"role": "tool",      "tool_call_id":"call_1", "content":"文件内容..."}
]
```

## 3. 初始化与生命周期

```c
int  AgentSessionStateInit(void);               /* 清空单例，初始化各子系统 */
int  AgentSessionStateOpen(void);               /* 打开新会话（必要时保存旧会话） */
void AgentSessionStateClose(void);              /* 关闭当前会话，保存历史 */
```

### 3.1 打开会话（AgentSessionStateOpen）

```
AgentSessionStateOpen():
    if inUse: return ALREADY_IN_USE   # 单例，同一时刻仅一个会话
    inUse = true
    messagesLen = 0
    # 可选：恢复此前该 sessionId 的持久化历史
    return OK
```

### 3.2 绑定用户与会话（BindUser）

```c
int AgentSessionStateBindUser(const char *userId);
```

设置 `userId`（来源为消息中的 userId，缺省可能去规范化到 "anonymous"）。若会话已有历史且属于另一用户，需要清空或按策略处理，避免跨用户上下文串扰。

### 3.3 加载与保存

```c
const char *AgentSessionStateLoad(void);      /* 返回 messagesJson 内容 */
int  AgentSessionStateSave(const char *messagesJson);  /* 拷贝保存 */
void AgentSessionStateClose(void);            /* 关闭：保存 + 复位 */
```

- `Load()`：把当前内存中的历史 JSON 提供给调用方（Agent 主循环）。
- `Save()`：把新的历史 JSON 拷贝进 `messagesJson`，并尝试**持久化**到磁盘（按 sessionId 命名文件），以便会话恢复。

## 4. 历史裁剪（TrimMessagesJson）—— 核心算法

当追加消息后总字节数超限，或条数超限时，执行裁剪。**保留最近的消息**。

```
TrimMessagesJson(messagesJson, messagesLen):
    # 输入: 完整消息数组（JSON 字符串），输出: 裁剪后的数组
    repeat:
        if byteLen(messagesJson) <= AGENT_MAX_CONVERSATION_BYTES
           and count(messagesJson) <= AGENT_MAX_CONVERSATION_MESSAGES:
            break

        # 去掉数组最前面的第一条消息
        removeFirstMessage(messagesJson)

        # 安全措施：优先丢弃 "system" 角色消息的冗余，
        # 但必须保留至少一条 user/assistant 结构化上下文，
        # 若只剩一条则停止
        if count <= 1: break
    return messagesJson
```

要点：
- 双上限：**字节（24KB）** 与 **条数（32 条）**，任一超限触发裁剪。
- 使用已有的 cJSON 对数组解析后进行逐条删除最保守：将数组序列化回字符串。
- 裁剪在**保存前**和**每次 LLM 轮次前**都会考虑，保证发送给 LLM 的上下文有界。

## 5. 单例与线程安全

该模块是静态单例，且**仅在 AgentThread 单线程内访问**（消息处理和 LLM 迭代都在该线程）。因此无需加锁；若未来多会话并发，需引入互斥量。

## 6. 关键接口签名汇总

```c
/* session_state.h */
int  AgentSessionStateInit(void);
int  AgentSessionStateOpen(void);
void AgentSessionStateClose(void);
int  AgentSessionStateBindUser(const char *userId);
const char *AgentSessionStateGetUserId(void);
const char *AgentSessionStateGetSessionId(void);
const char *AgentSessionStateGetInternal(void);   /* 返回内部 messagesJson 指针（只读） */
const char *AgentSessionStateLoad(void);
int  AgentSessionStateSave(const char *messagesJson);
void AgentSessionStateSetSessionId(const char *sessionId);
void AgentSessionStateTrimMessages(void);         /* 触发裁剪 */
```

## 7. 常数与边界

| 常量 | 值 | 说明 |
|------|-----|------|
| `AGENT_MAX_CONVERSATION_BYTES` | 24KB | 历史数组最大字节数 |
| `AGENT_MAX_CONVERSATION_MESSAGES` | 32 | 历史最大消息条数 |
| `AGENT_MAX_SESSION_ID_LEN` | 64 | sessionId 长度上限 |
| `AGENT_MAX_USER_ID_LEN` | 64 | userId 长度上限 |

边界：
- 若单条消息本身 > 24KB，裁剪后仍可能超限；此时需按「丢弃超长非关键消息」策略兜底，保证数组内部合法。
- `messagesJson` 必须始终是合法的 JSON 数组字符串，任何追加失败都不能破坏该不变量。

## 8. 复现要点（检查清单）

- [ ] 单例初始化后字段全部清零，`inUse=false`。
- [ ] `Open` 在已占用时返回错误，保证单会话语义。
- [ ] `Save` 后 `Load` 返回内容一致。
- [ ] 超过 32 条或 24KB 时，`Trim` 正确删去最旧消息并保持 JSON 合法。
- [ ] 关闭会话后状态复位，可再次打开。

## 9. 相关文档

- `kernel_agent_loop.md`：会话状态在主循环中的用法。
- `kernel_util.md`：涉及消息 JSON 构造与常量。
