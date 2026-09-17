# kernel_llm：LLM HTTP 客户端与流式解析

## 1. 模块职责

`kernel/llm/llm_api` 提供与 LLM 服务端交互的底层能力：

1. 读取并解析 LLM 配置（baseUrl / host / port / path / apiKey / model / 采样参数 / 流式开关）。
2. 基于**原生 TCP socket** 构造 HTTP/1.1 请求，不依赖 libcurl 等第三方 HTTP 库。
3. 发送 OpenAI 兼容的 Chat Completions 请求（含 tools）。
4. 解析响应：支持**非流式**（完整 JSON）与**流式**（SSE，增量拼接）两种模式。
5. 从响应中提取最终文本与工具调用（可多个）。
6. 失败自动**重试 3 次**，返回统一的负错误码。

## 2. 关键数据结构

### 2.1 配置（LlmConfig）

```c
typedef struct {
    const char *baseUrl;    /* 例如 https://api.xxx.com/v1 */
    char host[256];         /* 解析出的主机名 */
    int  port;              /* 端口（https 默认 443，http 默认 80） */
    char path[512];         /* 请求路径，如 /v1/chat/completions */
    int  useTls;            /* 是否启用 TLS */
    const char *apiKey;     /* API Key */
    const char *model;      /* 模型名 */
    int maxTokens;          /* 生成上限 */
    double temperature;     /* 采样温度 */
    int stream;             /* 是否流式 */
    const char *reasoningEffort; /* 推理强度（可选） */
} LlmConfig;
```

### 2.2 响应（LlmResponse）

```c
typedef struct {
    char text[/* 大缓冲或动态 */];      /* 最终组装文本 */
    size_t textLen;
    LlmToolCall calls[8];   /* 工具调用数组（上限 8） */
    int  callCount;         /* 工具调用数量 */
    int  toolUse;           /* 是否存在工具调用 */
    int  promptTokens;
    int  completionTokens;
    int  totalTokens;
} LlmResponse;
```

### 2.3 工具调用（LlmToolCall）

```c
typedef struct {
    char id[64];            /* 调用 id，如 call_xxx */
    char name[64];          /* 工具名，如 read */
    const char *input;      /* 参数 JSON 字符串（arguments） */
    size_t inputLen;
} LlmToolCall;
```

`input` 指向响应缓冲内的子串，不拥有内存。

## 3. 初始化（LlmInit/LlmConfigLoad）

```
LlmInit(config):
    if config 为 NULL: 从默认配置源加载（config/llm_config.json 或环境）
    解析 baseUrl -> host/port/path/useTls
    校验必填字段（model、apiKey 等）
    保存到全局 LlmConfig
```

基础 URL 解析规则：
- `https://` 前缀 -> useTls=1，端口默认 443；`http://` -> 端口默认 80。
- 去掉 scheme，剩余部分以第一个 `/` 分割：前面是 host[:port]，后面是 path。
- path 缺省为 `/v1/chat/completions`；若 baseUrl 已含 `/v1` 则保留。

## 4. 请求构造（LlmBuildChatToolsRequestJson）

拼装 JSON 请求体：

```
{
  "model": "<model>",
  "messages": [ ...来自调用方的 messagesJson... ],
  "tools": [ <g_toolsJson 内容> ],
  "tool_choice": "auto",
  "stream": <true/false>,
  "max_tokens": <maxTokens>,
  "temperature": <temperature>,
  "(可选) reasoning_effort": <reasoningEffort>
}
```

辅助函数：
- `ConvertMessagesOpenai`：把 messagesJson 数组元素标准化为 OpenAI 要求的字段。
- `ParseOpenaiToolsJson`：校验/规范化 tools 数组（从参数传入，通常是全局 `g_toolsJson`）。

## 5. 底层 HTTP 传输（HttpPostJson）—— 核心算法

不使用 HTTP 库，直接通过 socket 发送 HTTP/1.1 POST 请求。

```
HttpPostJson(host, port, path, headers, body):
    # 1) 解析主机
    addrinfo = getaddrinfo(host, ...)
    # 2) 创建 socket，连接
    sock = socket(AF_INET, SOCK_STREAM)
    set receive/send timeout（网络超时）
    setsockopt(TCP_NODELAY)
    connect(sock, addr)
    # 3) 发送请求
    write(sock,
        "POST <path> HTTP/1.1\r\n"
        "Host: <host>\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer <apiKey>\r\n"
        "Content-Length: <len>\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<body>")
    # 4) 读取响应
    loop:
        read(sock, buf)
        append to response buffer (动态扩展)
    返回原始 HTTP 响应
```

### 5.1 响应完整性判定（IsHttpResponseComplete）

持续读取直到满足：
- 已读到响应头末尾 `\r\n\r\n`；
- 且 body 长度达到 `Content-Length` 指定值；
- 若为 chunked 编码，则已读到所有 chunk 与结束标记。

### 5.2 分块解码（DecodeChunked）

对 `Transfer-Encoding: chunked` 的响应：

```
DecodeChunked(input):
    逐行读取十六进制 chunk 大小（忽略 chunk extensions）
    按大小读取对应字节并追加到输出
    直到读到大小为 0 的终止 chunk
    返回完整 body
```

## 6. SSE 流式解析（ExtractAnswerFromSseBody）

流式响应的 body 是若干 `data: {...}\n\n` 事件。逐行扫描：

```
ExtractAnswerFromSseBody(body):
    for each line starting with "data: ":
        json = parse(line after "data:")
        if json has choices[0].delta:
            content        += delta.content
            reasoning      += delta.reasoning or delta.reasoning_content
            # 工具调用（对每个 index 增量拼接）见下节
        # 遇到 "data: [DONE]" 结束
    返回 (text = content + reasoning 按模型约定拼接, toolCalls)
```

工具调用解析要点：**按 index 增量拼接 arguments**。

```
ExtractToolCallsFromSse(body):
    for each delta:
        if delta.tool_calls:
            for each tc in delta.tool_calls:
                idx = tc.index
                if tc.id:    calls[idx].id = tc.id
                if tc.function.name: calls[idx].name = tc.function.name
                if tc.function.arguments:
                    calls[idx].input 追加 tc.function.arguments 字符串
```

由于流式事件会把参数 JSON 切碎成多段，必须逐段 concat，最终得到完整 arguments JSON 字符串。

## 7. 非流式解析

当 `stream=false` 时，body 为完整 JSON：
- 取 `choices[0].message.content` 作为文本。
- 取 `choices[0].message.tool_calls[]` 作为工具调用（id / function.name / function.arguments）。
- `usage` 填充 token 统计。

## 8. 主入口 LlmChatToolsEx

```
LlmChatToolsEx(messagesJson, toolsJson, outResponse):
    for attempt in 0..2:               # 最多重试 3 次
        body = LlmBuildChatToolsRequestJson(...)
        resp = HttpPostJson(...)

        if resp 超时/连接失败: continue   # 重试
        if HTTP 状态非 2xx: 
            记录 Provider 错误; continue/返回对应错误码

        if stream:
            parse SSE body -> outResponse
        else:
            parse JSON body -> outResponse

        if 解析成功: 
            统计 token; return OK
    return ERROR
```

### 8.1 错误码约定

| 错误码 | 含义 |
|--------|------|
| -200 | 通用 LLM 调用失败 |
| -201..-205 | 连接/超时/解析等细分错误 |
| -206 | 请求内容过长（如超 token / 上下文超限） |
| -207 | Provider 错误（如 4xx/5xx） |

捕获 -206（过长）场景：上游调用方（主循环）可据此裁剪上下文后重试。

## 9. 关键算法与边界

### 9.1 文本与推理拼接
流式时把 `delta.reasoning`（或 `reasoning_content`）与 `delta.content` 都收集起来。**是否/如何合并**由调用方决定：`ExtractAnswerFromSseBody` 返回二者，主循环通过 `SkillHandlerExtractFinalText` 决定最终展示文本（通常只留 model 输出的可见 content）。

### 9.2 工具调用数量上限
`calls[8]` 上限 8：超过则取前 8，`callCount=8`。与 Agent 迭代上限一致。

### 9.3 动态缓冲
SSE body 与 arguments 拼接都使用动态扩展缓冲（`SseDynBuf`），避免流式长文本溢出。

### 9.4 超时
socket 收发设置超时（网络超时毫秒数），超时视为一个尝试失败计入重试。

## 10. 对外关键接口签名

```c
/* llm_api.h */
int  LlmInit(const LlmConfig *config);
int  LlmChatToolsEx(const char *messagesJson, const char *toolsJson, LlmResponse *response);
int  LlmBuildChatToolsRequestJson(const char *messagesJson, const char *toolsJson,
                                   int stream, char *out, size_t outSize);
```

## 11. 复现要点（检查清单）

- [ ] 能正确从 baseUrl 解析 host/port/path/useTls。
- [ ] 原生 socket 发送 HTTP POST 并解析 Content-Length / chunked。
- [ ] 非流式完整 JSON 解析正确。
- [ ] 流式 SSE：文本 delta 累加正确，工具 arguments 按 index 精确拼接。
- [ ] 空文本但带 tool_calls 时 `toolUse=1`、`callCount>0`。
- [ ] 网络异常自动重试 3 次，并返回统一负错误码。

## 12. 相关文档

- `kernel_llm_tool_adapter.md`：tools JSON 渲染与调用解析。
- `kernel_agent_loop.md`：LLM 在主循环中的调用与迭代。
