# PLC_Diagnosis 工具输出字段与 LLM 过滤方案

> 本文是 [`plc_diagnosis_module_architecture.md`](plc_diagnosis_module_architecture.md) 的 PLC 工具字段级补充，保留用于约束当前 Skill 脚本向 LLM 暴露的数据；模块边界、调用流程和部署方式以模块架构文档为入口。

## 1. 目的

本文说明 `skills/PLC_Diagnosis/scripts` 中每条命令当前实际返回给 LLM 的字段，以及建议保留、归一化或删除的字段。

过滤不能固定为“只保留信号 8201”。8201 只是当前通信异常 C001 分支使用的频段信号。后续可能查询多个设备、多个信号或新增诊断分支，因此过滤依据应来自本次命令参数和诊断上下文：

1. 调用方明确指定设备或信号时，返回这些目标对应的数据。
2. 调用方只指定设备和信号组、没有指定具体信号时，返回接口实际查询到的全部信号的核心字段。
3. 当前诊断分支可以标记重点信号，但不能删除其他已查询信号。
4. 大数组使用数量、分页和 `truncated` 标记控制，不能静默丢弃。
5. 原始响应用于本地审计，不直接进入 LLM 上下文。

## 2. 当前数据进入 LLM 的路径

ASP 请求当前由 `do_get`/`do_post` 输出一行 JSON：

```json
{
  "ok": true,
  "http_status": 200,
  "url": "完整请求 URL",
  "body": "完整 ASP 响应体，作为转义后的字符串"
}
```

POST 还可能带有 `stage`。`main` 将这些内容写入 stdout，`exec_program` 再包装为 Tool 结果：

```text
tool: exec_program
success: true
exit_code: 0
summary:
  tool completed
content:
<main 的完整 stdout>
truncated: false
raw_ref:
```

因此 LLM 当前看到两层信息：

- Runtime 层：`success`、`exit_code`、`summary`、`truncated`、`raw_ref`；
- PLC 脚本层：`ok`、`http_status`、`url`、完整 `body`，以及组合命令产生的阶段记录。

ASP 响应缓冲允许最大 1 MiB，但 Agent 单个工具输出缓冲只有 16 KiB。过大的 stdout 最终可能被截断，或者加速触发 `conversation is too large`。

## 3. 建议的统一输出协议

每次命令只向 stdout 输出一个 JSON 对象：

```json
{
  "schema_version": 1,
  "operation": "get_monitor_info",
  "success": true,
  "exit_code": 0,
  "transport": {
    "http_status": 200
  },
  "business": {
    "code": 0
  },
  "query": {},
  "data": {},
  "warnings": [],
  "error": null,
  "raw_response": {
    "saved": false,
    "bytes": 1345
  }
}
```

通用字段定义：

| 字段 | 是否发送 | 说明 |
|---|---|---|
| `schema_version` | 是 | 便于后续扩展字段而不破坏 LLM 解析 |
| `operation` | 是 | 实际命令名 |
| `success` | 是 | 综合参数、网络、HTTP、业务码和本地解析后的命令结果 |
| `exit_code` | 是 | 保留现有退出码语义 |
| `transport.http_status` | 有 HTTP 响应时发送 | 网络失败没有真实状态码时为 `null`，不要伪造 0 为 HTTP 状态 |
| `business.code` | 接口提供时发送 | 保留原始业务码并同时给出归一化成功判断 |
| `query` | 是 | 返回本次查询范围，避免 LLM 混淆设备、时间和分页 |
| `data` | 成功或部分成功时发送 | 过滤后的业务信息 |
| `warnings` | 有警告时发送 | 截断、字段缺失、映射未知等非致命问题 |
| `error` | 失败时发送 | `kind`、`stage`、简短 `message`，必要时带业务码 |
| `raw_response` | 可选 | 只说明是否保存、字节数和本地引用；不包含原文 |
| `url` | 否 | 命令和 `query` 已能表达请求；URL 冗长且可能携带参数 |
| `body` | 否 | 不把完整原始响应交给 LLM |

## 4. `login`

### 当前发送字段

成功：

```json
{
  "ok": true,
  "http_status": 200,
  "url": ".../login.asp",
  "stage": "login"
}
```

失败：

```json
{
  "ok": false,
  "http_status": 0,
  "url": ".../login.asp",
  "body": "Authentication failed",
  "stage": "login"
}
```

登录原始 token 当前没有打印，这是正确行为。

### 建议发送字段

```json
{
  "schema_version": 1,
  "operation": "login",
  "success": true,
  "exit_code": 0,
  "transport": {"http_status": 200},
  "data": {"authenticated": true},
  "error": null
}
```

失败时保留：`exit_code`、真实 `http_status`（如果有）、`error.kind=authentication|network|http|parse`、`error.stage=login` 和简短错误信息。

禁止发送：用户名、密码、token、Cookie、登录原始响应和完整 URL。

## 5. `get_cache_info`

### 当前发送字段

当前代码没有解析响应，直接发送：

- `ok`
- `http_status`
- `url`
- 完整 `body`

仓库内没有该接口的明确响应字段契约，所以目前无法可靠列出 `body` 内每个字段的业务含义。

### 建议发送字段

在取得真实响应样本并补齐接口契约前，不应猜测字段含义。目标结构应为：

```json
{
  "schema_version": 1,
  "operation": "get_cache_info",
  "success": true,
  "exit_code": 0,
  "transport": {"http_status": 200},
  "business": {"code": 0},
  "query": {
    "requested_device_ids": [1, 4099]
  },
  "data": {
    "returned_device_count": 2,
    "devices": [
      {
        "equip_id": 1,
        "equip_type_id": 33028,
        "equip_name": "Logger(Local)",
        "connectivity_state": "online",
        "last_update_time": "..."
      }
    ],
    "truncated": false
  }
}
```

设备字段应以真实接口定义为准。只保留设备身份、连通状态、状态时间和诊断流程明确需要的状态码；缓存内部实现字段、重复描述和大块原始数据不发送。

## 6. `get_active_alarm`

### 当前发送字段

外层字段：

- `ok`
- `http_status`
- `url`
- `body`

`body` 内：

- `errcode`
- `almlist[]`

当前实机 `almlist[]` 包含：

- `seqno`
- `almid`
- `almname`
- `equipid`
- `equiptypeid`
- `equipname`
- `level`
- `confirmstate`
- `reason`
- `reasonum`
- `position`
- `description`
- `type`
- `localtime`
- `lockstate`
- `locationInfo`
- `faultDesc`
- `subReasonList`

### 建议发送字段

```json
{
  "schema_version": 1,
  "operation": "get_active_alarm",
  "success": true,
  "exit_code": 0,
  "transport": {"http_status": 200},
  "business": {"code": 0},
  "query": {
    "equip_id": 0,
    "target_alarm": {
      "almid": 1154,
      "equipid": 1,
      "seqno": 24,
      "localtime": "2026-09-08 10:56:52"
    },
    "diagnostic_problem": "communication_anomaly"
  },
  "data": {
    "total_alarm_count": 12,
    "relevant_alarm_count": 2,
    "alarms": [
      {
        "seqno": 24,
        "almid": 1154,
        "almname": "Southbound device communication anomaly",
        "equipid": 1,
        "equiptypeid": 33028,
        "equipname": "Logger(Local)",
        "level": 2,
        "reason": 671,
        "reasonum": 1,
        "position": "...",
        "localtime": "2026-09-08 10:56:52",
        "faultDesc": "The inverter communication is abnormal.",
        "topCauses": [
          {"rank": 1, "reason": "...", "repair": "..."},
          {"rank": 2, "reason": "...", "repair": "..."},
          {"rank": 3, "reason": "...", "repair": "..."}
        ],
        "relevance": {
          "target_occurrence_match": true,
          "problem_scope_match": true
        }
      }
    ],
    "unrelated_alarm_count": 10,
    "truncated": false
  }
}
```

保留规则：

- 入口核验必须保留 `seqno + almid + equipid + localtime`。
- 问题识别保留 `almname + faultDesc + reason + reasonum`。
- 设备映射保留 `equipid + equiptypeid + equipname`。
- `position`、`locationInfo` 在能帮助确定故障范围时保留；空值删除。
- 不发送完整 `description`。实机响应中的该字段是约5 KiB的故障码原因/解决办法全集，不只描述当前告警。
- 对每条相关告警，从当前告警对应的原因和解决办法中按原始顺序取前3组，归一化为 `topCauses[].rank/reason/repair`。
- 优先从 `subReasonList[].subReason/subRepair` 提取，因为实机数据已经按当前告警原因完成匹配；它缺失时，才按当前告警的原因标识从 `description` 提取对应故障码块。
- `subReason` 和 `subRepair` 内的编号列表按相同序号配对；不足3组时只返回实际存在的组，原因或办法一侧缺失时保留该项并将缺失侧设为 `null`。
- 不把原始 `description` 或原始 `subReasonList` 再发送给LLM。
- `confirmstate`、`lockstate`、`type` 只有诊断规则实际使用时才保留。

过滤对象是“与本次目标告警、目标设备或诊断问题相关的告警”，不是固定某个告警ID。未指定诊断问题时，应返回所有活动告警的核心字段，通过分页或数量上限控制，而不是擅自只留下通信告警。

## 7. `get_monitor_info`

### 当前发送字段

外层字段：`ok`、`http_status`、`url`、完整 `body`。

`body` 中包含：

- `errCode`
- `sigList[]`

信号可能包含：

- `sigId`
- `sigName`
- `sigValue`
- `sigUnit`
- `sigLevel`
- `ctrlSigFlag`
- `sigAuth`
- `valType`
- `enumList`
- `defaultVal`
- `minVal`
- `maxVal`
- `valStep`
- `precision`
- `maxValLen`

### 建议发送字段

不能固定只返回 8201。建议结构：

```json
{
  "schema_version": 1,
  "operation": "get_monitor_info",
  "success": true,
  "exit_code": 0,
  "transport": {"http_status": 200},
  "business": {"code": 0},
  "query": {
    "devices": [
      {"equip_id": 4099, "equip_type_id": 33036}
    ],
    "para3": 4,
    "para4": 2,
    "requested_signal_ids": [8201, 8202],
    "diagnostic_focus_signal_ids": [8201]
  },
  "data": {
    "devices": [
      {
        "equip_id": 4099,
        "equip_type_id": 33036,
        "signals": [
          {
            "sigId": 8201,
            "sigName": "Network frequency band",
            "sigValue": 3,
            "displayValue": "Band 2",
            "sigUnit": "",
            "isDiagnosticFocus": true
          },
          {
            "sigId": 8202,
            "sigName": "Another signal",
            "sigValue": 10,
            "displayValue": "10 V",
            "sigUnit": "V",
            "isDiagnosticFocus": false
          }
        ]
      }
    ],
    "returned_signal_count": 2,
    "truncated": false
  }
}
```

信号保留规则：

1. 如果调用参数提供 `requested_signal_ids`，返回这些信号；缺失的目标也要通过 `missing_signal_ids` 明确报告。
2. 如果接口只能按 `para3/para4` 查询一组信号，没有指定具体信号ID，则返回该组全部信号的核心字段，不能只保留8201。
3. `diagnostic_focus_signal_ids` 只用于标记当前分支重点，不用于删除其他信号。
4. 每个信号默认保留 `sigId`、`sigName`、`sigValue`、`sigUnit` 和本地解析后的 `displayValue`。
5. 枚举信号应在本地用 `enumList` 计算 `displayValue`。通常不把完整 `enumList` 发给 LLM。
6. 如果任务是选择一个新的合法设置值，则额外返回精简后的 `allowedValues`；否则删除完整枚举表。
7. `minVal/maxVal/valStep/precision/maxValLen` 只在参数设置或范围判断任务中返回。
8. `sigAuth/ctrlSigFlag/valType/sigLevel` 只在权限、控制能力或类型判断中返回。
9. 多设备结果按设备分组，并保留设备ID和类型ID，防止信号归属混淆。

对于当前 C001，可以额外提供：

```json
{
  "derived": {
    "frequency_band": {
      "signal_id": 8201,
      "raw_value": 3,
      "band_number": 2,
      "valid": true
    }
  }
}
```

这是当前分支的派生结果，不应改变通用 `signals[]` 的返回规则。

## 8. `get_history_alarm`

### 当前发送字段

外层字段：`ok`、`http_status`、`url`、完整 `body`。

历史响应定义包括：

- `errcode`
- `totalnum`
- `hisalmlist[]`
  - `seqno`
  - `level`
  - `equipname`
  - `alarmname`
  - `startime`
  - `endtime`
  - `confirmstate`
  - `reason`
  - `reasonum`
  - `equipid`
  - `alarmed`（告警ID）
  - `equiptypeid`

### 建议发送字段

```json
{
  "schema_version": 1,
  "operation": "get_history_alarm",
  "success": true,
  "exit_code": 0,
  "transport": {"http_status": 200},
  "business": {"code": 0},
  "query": {
    "equip_id": 1,
    "start_time": "...",
    "end_time": "...",
    "page_index": 1,
    "page_size": 20,
    "alarm_level": 255,
    "diagnostic_problem": "communication_anomaly"
  },
  "data": {
    "total_count": 130,
    "returned_count": 20,
    "relevant_count": 3,
    "alarms": [
      {
        "seqno": 24,
        "alarm_id": 1154,
        "alarm_name": "Southbound device communication anomaly",
        "equipid": 1,
        "equiptypeid": 33028,
        "equipname": "Logger(Local)",
        "level": 2,
        "start_time": "...",
        "end_time": "...",
        "reason": 671,
        "reasonum": 1
      }
    ],
    "truncated": false
  }
}
```

过滤同样由查询目标决定：指定设备、告警或问题类型时返回相关项；未指定时保留当前页全部历史告警的核心字段，并通过较小的 `page_size`、分页和 `truncated` 控制大小。不能无条件只保留通信异常。

## 9. `set_freq_band`

### 当前发送字段

一次调用输出多行 JSON：

1. 修复前 `get_monitor_info` 的完整响应；
2. `frequency_plan`：`stage/from_band/to_band`；
3. 写入请求完整响应：`ok/http_status/url/body/stage`；
4. 修复后 `get_monitor_info` 的完整响应；
5. `verify_freq_band`：`stage/ok`。

写入响应 `body` 包含：

- `errCode`
- `resultList[]`
  - `equipId`
  - `equipTypeId`
  - `result`
  - `sigResultList[]`
    - `sigId`
    - `setResult`

### 建议发送字段

`set_freq_band` 是一个专用组合命令，可以保留8201相关结果，因为命令本身的语义就是修改频段；但结构仍应支持多个设备返回结果：

```json
{
  "schema_version": 1,
  "operation": "set_freq_band",
  "success": true,
  "exit_code": 0,
  "target": {
    "equip_id": 4099,
    "equip_type_id": 33036,
    "signal_id": 8201
  },
  "before": {
    "raw_value": 1,
    "band_number": 1
  },
  "planned": {
    "raw_value": 3,
    "band_number": 2
  },
  "write": {
    "http_status": 200,
    "business_code": 0,
    "device_results": [
      {
        "equip_id": 4099,
        "equip_type_id": 33036,
        "result": 0,
        "signal_results": [
          {"sig_id": 8201, "set_result": 0}
        ]
      }
    ],
    "accepted": true
  },
  "after": {
    "raw_value": 3,
    "band_number": 2
  },
  "verification": {
    "readback_matches": true,
    "parameter_result": "success"
  },
  "error": null
}
```

代码必须在本地检查 `errCode`、目标设备结果、目标信号 `setResult` 和读回值，不能仅凭 HTTP 200 或让 LLM 从原始响应中自行判断。

## 10. 数量与截断规则

过滤不等于隐藏数据，推荐统一遵守：

- 所有数组返回 `total_count`、`returned_count` 和 `truncated`。
- 指定目标缺失时返回 `missing_device_ids` 或 `missing_signal_ids`。
- 被过滤的无关项目只返回数量，例如 `unrelated_alarm_count`，不返回完整内容。
- 字符串设置单项长度上限；被截断时增加 `value_truncated=true`。
- 超过单次输出预算时优先分页，不随机删除中间元素。
- 建议普通查询的最终 stdout 控制在 4 KiB 内；确需多设备、多信号时分页，而不是恢复完整原始响应。

## 11. 推荐实现边界

建议把职责分为三层：

1. `asp/*.c`：执行请求并返回原始 `Buffer`，不直接打印完整响应。
2. `main.c` 或新增 `normalize.c`：校验业务码、解析设备/信号/告警、执行上下文相关过滤，构造统一结果对象。
3. 输出层：只打印一个归一化 JSON；`--output` 可保存原始响应供本地审计。

其中通用查询与诊断派生结果应分开：

- `data.devices[].signals[]` 保留本次实际查询范围内的通用信号数据；
- `derived.frequency_band` 等字段表达当前诊断分支的重点解释；
- 新增诊断分支时增加新的 `derived` 字段，不修改通用数据过滤原则。

## 12. 各命令字段字典

本节是前述返回示例的字段说明。字段分为“始终返回”“成功时返回”“失败时返回”和“按任务返回”，实现时不应用空字符串填充所有可选字段。

### 12.1 `login`

| 字段 | 条件 | 含义 |
|---|---|---|
| `operation` | 始终 | 固定为 `login` |
| `success` | 始终 | HTTP成功且登录响应中取得有效token时为 `true` |
| `exit_code` | 始终 | 0成功；3网络失败；4 HTTP或接口失败；5认证失败 |
| `transport.http_status` | 收到HTTP响应 | 服务端真实HTTP状态；网络未连通时为 `null` |
| `data.authenticated` | 始终 | 是否完成认证，只返回布尔值 |
| `error.kind` | 失败 | `network/http/authentication/parse/local` |
| `error.stage` | 失败 | 固定为 `login` |
| `error.message` | 失败 | 不含凭据、token和原始响应的短错误说明 |

成功示例：

```json
{
  "schema_version": 1,
  "operation": "login",
  "success": true,
  "exit_code": 0,
  "transport": {"http_status": 200},
  "data": {"authenticated": true},
  "error": null
}
```

失败示例：

```json
{
  "schema_version": 1,
  "operation": "login",
  "success": false,
  "exit_code": 5,
  "transport": {"http_status": 401},
  "data": {"authenticated": false},
  "error": {"kind": "authentication", "stage": "login", "message": "Authentication failed"}
}
```

### 12.2 `get_cache_info`

该接口当前没有仓库内字段契约。下表描述目标语义，不代表已经确认原始ASP字段名；取得实机样本后必须补充“原始字段 → 归一化字段”映射。

| 字段 | 条件 | 含义 |
|---|---|---|
| `query.requested_device_ids` | 始终 | 本次关注的设备ID；空数组表示未指定具体设备 |
| `data.returned_device_count` | 成功 | 符合查询条件的设备数 |
| `data.devices[].equip_id` | 成功 | 设备ID，原始字段待确认 |
| `data.devices[].equip_type_id` | 成功 | 设备类型ID，原始字段待确认 |
| `data.devices[].equip_name` | 成功 | 设备名称，原始字段待确认 |
| `data.devices[].connectivity_state` | 成功 | 归一化为 `online/offline/unknown`，原始状态码待确认 |
| `data.devices[].last_update_time` | 接口提供时 | 缓存或连接状态最后更新时间 |
| `data.truncated` | 成功 | 是否只返回了部分设备 |

在协议尚未确认时，不能把猜测结果交给LLM，应明确返回解析失败：

```json
{
  "schema_version": 1,
  "operation": "get_cache_info",
  "success": false,
  "exit_code": 4,
  "transport": {"http_status": 200},
  "business": {"code": null},
  "query": {"requested_device_ids": []},
  "data": null,
  "warnings": ["cache response schema is not configured"],
  "error": {
    "kind": "parse",
    "stage": "normalize_response",
    "message": "Cannot safely interpret cache response"
  },
  "raw_response": {"saved": true, "bytes": 8421}
}
```

### 12.3 `get_active_alarm`

| 原始字段 | 建议字段 | 含义 |
|---|---|---|
| `errcode` | `business.code` | `OK`或数字0表示活动告警查询成功 |
| `almlist` | `data.alarms` | 根据本次设备、目标告警或问题范围筛选后的活动告警 |
| `seqno` | `seqno` | 本次告警发生序列号；同一告警再次发生时可能变化 |
| `almid` | `almid` | 告警定义ID |
| `almname` | `almname` | 告警名称 |
| `equipid` | `equipid` | 告警上报设备ID，不一定是参数修复设备ID |
| `equiptypeid` | `equiptypeid` | 告警上报设备类型ID |
| `equipname` | `equipname` | 告警上报设备名称 |
| `level` | `level` | 告警等级 |
| `confirmstate` | 同名，按需 | 告警确认状态；当前流程不用时省略 |
| `reason` | `reason` | 原始原因字段；实机样本中包含原因代码671 |
| `reasonum` | `reasonum` | 另一原因标识；文档与实机语义完全核清前同时保留 |
| `position` | `position` | 故障位置或实例范围 |
| `description` | 不直接发送 | 实机字段包含多个Fault Code的原因/解决办法全集；只作为 `topCauses` 的后备解析来源 |
| `localtime` | `localtime` | 告警发生时间，也是推送入口身份核验字段 |
| `faultDesc` | `faultDesc` | 具体故障描述，用于问题分类和复查 |
| `subReasonList[].subReason` | `topCauses[].reason` | 当前告警的编号原因列表，按原始顺序最多取前3条 |
| `subReasonList[].subRepair` | `topCauses[].repair` | 与原因序号对应的解决办法，按相同序号配对 |
| 无 | `topCauses[].rank` | 原始顺序，从1开始；表示接口顺序而不是模型重新评分 |
| `locationInfo` | 同名，按需 | 非空且有助于确定故障范围时保留 |
| `type/lockstate` | 按需 | 当前P001不使用，默认省略 |
| 无 | `relevance.target_occurrence_match` | 是否同时匹配目标 `almid/equipid/seqno/localtime` |
| 无 | `relevance.problem_scope_match` | 是否匹配当前诊断问题和设备范围 |
| 无 | `data.total_alarm_count` | 原始活动告警总数 |
| 无 | `data.relevant_alarm_count` | 返回给LLM的相关告警数 |
| 无 | `data.unrelated_alarm_count` | 未展开的无关告警数 |

没有发现目标问题时的示例：

```json
{
  "schema_version": 1,
  "operation": "get_active_alarm",
  "success": true,
  "exit_code": 0,
  "transport": {"http_status": 200},
  "business": {"code": 0},
  "query": {"equip_id": 1, "diagnostic_problem": "communication_anomaly"},
  "data": {
    "total_alarm_count": 4,
    "relevant_alarm_count": 0,
    "alarms": [],
    "unrelated_alarm_count": 4,
    "truncated": false
  },
  "error": null
}
```

### 12.4 `get_monitor_info`

| 原始字段 | 建议字段 | 含义及发送条件 |
|---|---|---|
| `errCode` | `business.code` | 监控接口业务码，0表示成功 |
| `sigList` | `data.devices[].signals` | 本次设备和信号组返回的信号集合 |
| `sigId` | `sigId` | 信号ID，始终保留 |
| `sigName` | `sigName` | 信号名称，始终保留 |
| `sigValue` | `sigValue` | 原始接口值，保持原始数值/字符串类型 |
| `sigUnit` | `sigUnit` | 信号单位；空值可省略 |
| `enumList` | `displayValue` | 本地匹配枚举值后得到的可读名称 |
| `enumList` | `allowedValues`，按需 | 只有参数设置或候选值选择任务才发送精简枚举列表 |
| `minVal/maxVal` | `constraints.min/max`，按需 | 参数范围判断或设置任务使用 |
| `valStep` | `constraints.step`，按需 | 参数设置步长 |
| `precision` | `constraints.precision`，按需 | 数值精度 |
| `maxValLen` | `constraints.max_length`，按需 | 字符串最大长度 |
| `sigAuth` | `capability.auth`，按需 | 需要判断信号权限时发送 |
| `ctrlSigFlag` | `capability.control_flag`，按需 | 需要判断是否可控或提示行为时发送 |
| `valType` | `value_type`，按需 | 需要区分枚举、数值或字符串时发送 |
| `sigLevel` | `signal_level`，按需 | 查询分层或权限规则需要时发送 |
| 无 | `isDiagnosticFocus` | 当前诊断重点标记，不能作为删除其他信号的依据 |
| 无 | `missing_signal_ids` | 明确请求但接口没有返回的信号ID |
| 无 | `returned_signal_count` | 本次返回的信号数量 |
| 无 | `truncated` | 是否因分页或输出预算仅返回部分信号 |

没有指定具体信号ID时，应返回该查询组内所有信号的核心字段：

```json
{
  "schema_version": 1,
  "operation": "get_monitor_info",
  "success": true,
  "exit_code": 0,
  "transport": {"http_status": 200},
  "business": {"code": 0},
  "query": {
    "devices": [{"equip_id": 4099, "equip_type_id": 33036}],
    "para3": 4,
    "para4": 2,
    "requested_signal_ids": [],
    "diagnostic_focus_signal_ids": [8201]
  },
  "data": {
    "devices": [{
      "equip_id": 4099,
      "equip_type_id": 33036,
      "signals": [
        {"sigId": 8201, "sigName": "Network frequency band", "sigValue": 3, "displayValue": "Band 2", "isDiagnosticFocus": true},
        {"sigId": 8210, "sigName": "Signal A", "sigValue": 48, "sigUnit": "V", "isDiagnosticFocus": false},
        {"sigId": 8211, "sigName": "Signal B", "sigValue": 6, "displayValue": "Enabled", "isDiagnosticFocus": false}
      ]
    }],
    "returned_signal_count": 3,
    "missing_signal_ids": [],
    "truncated": false
  },
  "derived": {
    "frequency_band": {"signal_id": 8201, "raw_value": 3, "band_number": 2, "valid": true}
  },
  "error": null
}
```

上例中的8201只属于 `diagnostic_focus_signal_ids` 和 `derived.frequency_band`，其他实际查询到的信号仍保留。

### 12.5 `get_history_alarm`

| 原始字段 | 建议字段 | 含义 |
|---|---|---|
| `errcode` | `business.code` | 数字0表示历史告警查询成功 |
| `totalnum` | `data.total_count` | 查询条件下的历史告警总数，不是当前页数量 |
| `hisalmlist` | `data.alarms` | 当前页中按上下文过滤或精简后的历史告警 |
| `seqno` | `seqno` | 历史告警发生序列号 |
| `alarmed` | `alarm_id` | 历史接口中的告警ID，归一化名称避免误解 |
| `alarmname` | `alarm_name` | 告警名称 |
| `equipid` | `equipid` | 告警设备ID |
| `equiptypeid` | `equiptypeid` | 告警设备类型ID |
| `equipname` | `equipname` | 告警设备名称 |
| `level` | `level` | 告警等级 |
| `startime` | `start_time` | 告警开始时间 |
| `endtime` | `end_time` | 告警结束时间；空值表示接口未提供 |
| `confirmstate` | 同名，按需 | 告警确认状态 |
| `reason/reasonum` | 同名 | 原始原因字段，语义核清前同时保留 |
| 无 | `data.returned_count` | 当前页接口实际返回数 |
| 无 | `data.relevant_count` | 当前页符合过滤条件的数量 |
| 无 | `data.truncated` | 是否还有告警未返回；存在后续分页时也应为 `true` |

未指定问题类型时，不做“通信异常”过滤，返回当前页全部核心字段：

```json
{
  "schema_version": 1,
  "operation": "get_history_alarm",
  "success": true,
  "exit_code": 0,
  "transport": {"http_status": 200},
  "business": {"code": 0},
  "query": {
    "equip_id": 1,
    "start_time": "2026-09-01-00-00-00",
    "end_time": "2026-09-10-23-59-59",
    "page_index": 1,
    "page_size": 2,
    "diagnostic_problem": null
  },
  "data": {
    "total_count": 18,
    "returned_count": 2,
    "relevant_count": 2,
    "alarms": [
      {"seqno": 24, "alarm_id": 1154, "alarm_name": "Southbound device communication anomaly", "equipid": 1, "equiptypeid": 33028, "equipname": "Logger(Local)", "level": 2, "start_time": "2026-09-08 10:56:52", "end_time": "", "reason": 671, "reasonum": 1},
      {"seqno": 23, "alarm_id": 1002, "alarm_name": "Example alarm", "equipid": 1, "equiptypeid": 33028, "equipname": "Logger(Local)", "level": 1, "start_time": "2026-09-07 08:10:00", "end_time": "2026-09-07 08:15:00", "reason": 12, "reasonum": 1}
    ],
    "truncated": true
  },
  "error": null
}
```

### 12.6 `set_freq_band`

| 字段 | 来源 | 含义 |
|---|---|---|
| `target.equip_id/equip_type_id` | 命令参数 | 实际修改的参数设备，不是告警上报设备 |
| `target.signal_id` | 命令语义 | `set_freq_band` 当前固定修改频段信号8201 |
| `before.raw_value` | 写入前监控响应 | 修改前接口枚举值 |
| `before.band_number` | 本地映射 | 修改前Band编号 |
| `planned.raw_value` | 本地计划 | 准备写入的接口枚举值 |
| `planned.band_number` | 本地计划 | 准备切换到的Band编号 |
| `write.http_status` | 写入响应 | 写请求HTTP状态 |
| `write.business_code` | `errCode` | 写入接口整体业务码 |
| `write.device_results[].result` | `resultList[].result` | 单个设备设置结果，0表示成功 |
| `write.signal_results[].set_result` | `sigResultList[].setResult` | 单个信号设置结果，0表示成功 |
| `write.accepted` | 本地综合判断 | 业务码、目标设备和目标信号结果是否全部成功 |
| `after.raw_value/band_number` | 写入后监控响应 | 实际读回值及Band编号 |
| `verification.readback_matches` | 本地比较 | 读回值是否等于计划值 |
| `verification.parameter_result` | 本地结论 | `success/failed/unknown/not_executed` |
| `error.stage` | 本地阶段 | `read_before/plan/write/readback` |

写入失败示例：

```json
{
  "schema_version": 1,
  "operation": "set_freq_band",
  "success": false,
  "exit_code": 4,
  "target": {"equip_id": 4099, "equip_type_id": 33036, "signal_id": 8201},
  "before": {"raw_value": 1, "band_number": 1},
  "planned": {"raw_value": 3, "band_number": 2},
  "write": {
    "http_status": 200,
    "business_code": 0,
    "device_results": [{
      "equip_id": 4099,
      "equip_type_id": 33036,
      "result": 1,
      "signal_results": [{"sig_id": 8201, "set_result": 5}]
    }],
    "accepted": false
  },
  "after": null,
  "verification": {"readback_matches": null, "parameter_result": "failed"},
  "error": {"kind": "business", "stage": "write", "message": "Device rejected frequency-band update"}
}
```

## 13. 现有日志上下文压缩实测

测量方法：使用 `logs/agent_trace_20260909_064215.jsonl` 中的真实 `exec_program` 输出，比较当前脚本stdout作为LLM Tool消息再次JSON转义后的UTF-8字节数，与本文建议归一化JSON按相同方式转义后的字节数。该指标接近实际上下文占用字节，但不等同于精确token数；不同模型的tokenizer会有差异。

| 命令/方案 | 当前进入LLM | 过滤后进入LLM | 减少字节 | 减少比例 |
|---|---:|---:|---:|---:|
| `get_active_alarm`：2条告警，核心字段 + 每条Top 3原因/办法 | 12,943 B | 1,943 B | 11,000 B | 85.0% |
| `get_active_alarm`：2条告警，仅核心字段 | 12,943 B | 1,062 B | 11,881 B | 91.8% |
| `get_active_alarm`：只返回目标告警 + Top 3原因/办法 | 12,943 B | 1,284 B | 11,659 B | 90.1% |
| `get_monitor_info`：保留查询组全部3个信号的核心字段 | 1,730 B | 741 B | 989 B | 57.2% |
| `set_freq_band`：合并5条阶段记录为一个结果 | 3,869 B | 601 B | 3,268 B | 84.5% |

活动告警样本中，每条告警的完整 `description` 约5,286字节，两条约10.6 KiB，是当前输出的主要体积来源。如果保留完整 `description`，同一方案只能减少约2.5%；因此必须提取Top 3后删除原始字段。

当前样本每条告警分别只有3组和2组已匹配的 `subReason/subRepair`，所以“Top 3”保留了它们全部的当前告警原因和处理办法，没有丢失该样本的有效条目。如果以后接口返回超过3组，则按接口原始顺序取前3组，并通过以下字段说明裁剪：

```json
{
  "cause_repair_total_count": 7,
  "topCauses": [
    {"rank": 1, "reason": "...", "repair": "..."},
    {"rank": 2, "reason": "...", "repair": "..."},
    {"rank": 3, "reason": "...", "repair": "..."}
  ],
  "cause_repair_truncated": true
}
```

暂时不能给出真实压缩比例的命令：

- `get_history_alarm`：现有trace中没有可解析的真实 `hisalmlist` 响应样本；比例取决于单页告警数和每条文本长度。
- `get_cache_info`：既没有真实响应样本，也没有明确字段契约，必须先取得样本并确认语义。
- `login`：当前成功输出已经很小，过滤重点是安全和统一协议，不以减少上下文为主要收益。

这里没有为了测量重新调用实机写接口；`set_freq_band` 数据来自已有日志，避免再次修改设备参数。
