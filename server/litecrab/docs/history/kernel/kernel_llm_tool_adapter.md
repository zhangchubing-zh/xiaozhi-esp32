# kernel_llm_tool_adapter：工具目录渲染与工具调用解析

## 1. 模块职责

`kernel/llm/llm_tool_adapter` 是 **runtime 工具规格** 与 **OpenAI Chat Completions 协议** 之间的桥接层：

1. **渲染方向（runtime -> LLM）**：把运行时工具目录 `CrabToolCatalogView` 渲染为 OpenAI 风格的 `tools` JSON 数组（`type: "function"`，含 name / description / parameters）。
2. **解析方向（LLM -> runtime）**：把 LLM 返回的某个工具调用（callId + toolName + argumentsJson 字符串）解析为类型安全的 `CrabToolCall`（含类型化参数值）。

它完全基于 cJSON，不触碰网络层。

## 2. 关键数据结构

- 输入渲染：`CrabToolCatalogView`（runtime 契约层定义）。
- 输出渲染：`char *out` 缓冲（如全局 `g_toolsJson[65536]`）。
- 输入解析：`callId` / `toolName` / `argumentsJson`（字符串）。
- 输出解析：`CrabToolCall`（含 `CrabToolArg` 数组，见 runtime 文档）。

## 3. 渲染算法（RenderOpenaiToolsJson）

### 3.1 输出结构

每个工具生成：

```json
{
  "type": "function",
  "function": {
    "name": "<tool名>",
    "description": "<工具描述>",
    "parameters": {
      "type": "object",
      "properties": {
        "<参数名>": {
           "type": "string|integer|boolean",
           "description": "<参数描述>",
           "enum": [...],        // STRING 类型且有枚举时
           "minimum": n,          // INT 类型且有范围时
           "maximum": n
        },
        ...
      },
      "required": ["<必填参数名>", ...],
      "additionalProperties": false
    }
  }
}
```

### 3.2 算法流程

```
RenderOpenaiToolsJson(catalog, out, outSize):
    arr = cJSON_CreateArray()
    for each tool in catalog.tools:
        toolObj = object { type:"function" }
        funcObj = object
        parameters = object { type:"object" }
        properties = object{}
        required   = array[]

        for each param in tool.params:
            schema = object
            schema.type = ValueTypeName(param.type)   # "string"/"integer"/"boolean"
            schema.description = param.description
            if param.type==STRING && param.enumCount>0:
                schema.enum = [param.enumValues...]
            if param.type==INT && (minInt!=0 || maxInt!=0):
                schema.minimum = param.minInt
                schema.maximum = param.maxInt
            properties[param.name] = schema
            if param.required: required.append(param.name)

        parameters.properties = properties
        parameters.required   = required
        parameters.additionalProperties = false
        funcObj.name       = tool.name
        funcObj.description = tool.description
        funcObj.parameters = parameters
        toolObj.function   = funcObj
        arr.append(toolObj)

    json = cJSON_PrintUnformatted(arr)   # 紧凑无空白序列化
    copy to out (校验 outSize)
    cJSON_Delete(arr)
```

要点：
- `type` 映射：STRING->"string"，INT->"integer"，BOOL->"boolean"，未知默认 "string"。
- `additionalProperties:false` 强制模型只填已声明参数，与运行时校验严格一致。
- `required` 直接来源于 `param->required`。

### 3.3 返回值

| 返回值 | 含义 |
|--------|------|
| 0 | 成功 |
| -1 | 参数无效 |
| -2 | 创建数组失败 |
| -3 | 内存不足 |
| -4 | 参数 Schema 失败 |
| -3 | 序列化后超出缓冲区 |

## 4. 解析算法（ParseCall）

把 LLM 返回的调用解析为类型化 `CrabToolCall`：

```
ParseCall(catalog, callId, toolName, argumentsJson, call, error, errorSize):
    memset(call, 0)
    call.callId   = callId
    call.toolName = toolName

    tool = FindTool(catalog, toolName)
    if !tool: 设置错误 "tool not found"; return -1

    root = cJSON_Parse(argumentsJson ? argumentsJson : "{}")
    if !root 或非对象: "invalid tool arguments JSON"; return -1

    for each child in root (参数):
        if call.argCount >= CRAB_TOOL_MAX_ARGS:
            "too many tool arguments"; FreeCall; return -1

        param = 在 tool.params 中按 child->string 查找
        if !param:
            "unknown tool argument"; FreeCall; return -1

        FillArg(param, child, &call.args[argCount], ...)
           # STRING: 校验 cJSON_IsString，strdup->stringValue
           # INT:    cJSON_IsNumber, intValue = valuedouble
           # BOOL:   cJSON_IsBool,  boolValue = IsTrue
           类型不匹配 -> "string/integer/boolean argument expected"; FreeCall; return -1
        call.argCount++

    cJSON_Delete(root)
    return 0
```

### 4.1 FillArg 类型校验

- **STRING**：要求 `cJSON_IsString`，否则报错；把 `valuestring` 深拷贝为堆内存（不拥有调用方内存）。
- **INT**：要求 `cJSON_IsNumber`，`intValue = (int64_t)value->valuedouble`。
- **BOOL**：要求 `cJSON_IsBool`，`boolValue = cJSON_IsTrue(...)`。
- 其他类型：报 "unsupported argument type"。

### 4.2 内存所有权

解析后 `CrabToolArg.value.stringValue` 是**堆分配**的副本。使用完后必须调用 `LlmToolAdapterFreeCall(call)` 释放所有字符串参数并重置 `argCount`。这避免与运行时契约层（`CrabToolValue.stringValue` 不拥有内存）产生歧义。

## 5. 对外关键接口签名

```c
/* llm_tool_adapter.h */
int LlmToolAdapterRenderOpenaiToolsJson(const CrabToolCatalogView *catalog,
                                        char *out, size_t outSize);
int LlmToolAdapterParseCall(const CrabToolCatalogView *catalog,
                            const char *callId, const char *toolName,
                            const char *argumentsJson,
                            CrabToolCall *call, char *error, size_t errorSize);
void LlmToolAdapterFreeCall(CrabToolCall *call);
```

## 6. 复现要点（检查清单）

- [ ] 渲染输出为合法 OpenAI tools 数组，`additionalProperties=false`。
- [ ] 枚举、整型范围正确映射到 `enum` / `minimum` / `maximum`。
- [ ] `required` 数组与参数规格一致。
- [ ] 解析时未知参数名、类型不匹配、参数超上限均返回明确错误。
- [ ] `FreeCall` 正确释放字符串参数堆内存且不悬垂。

## 7. 相关文档

- `kernel_llm.md`：LLM 响应中工具调用如何在协议层产生。
- `runtime_overview.md`：`CrabToolCatalogView` / `CrabToolCall` 契约定义。
- `kernel_agent_loop.md`：工具执行器如何使用 ParseCall。
