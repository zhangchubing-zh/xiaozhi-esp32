# 本地 Agent 直跑设备端设计方案（方案 A：LocalAgentProtocol）

| 项目 | 内容 |
|---|---|
| 日期 | 2026-09-17 |
| 状态 | 设计阶段，待评审 |
| 范围 | 在 ESP32 固件内新增一个 `Protocol` 实现，把 agent loop（ASR → LLM 工具循环 → TTS 编排）从 PC 中转进程移到设备端，固件直接 HTTPS 调云端 ASR/LLM/TTS，不再依赖任何外置服务器进程 |
| 关联文档 | `docs/websocket_zh.md`（协议契约）；`docs/mcp-protocol_zh.md`（MCP）；`2026-09-14-xiaozhi-litecrab-agent-design.md`（前作 PC 中转方案，本方案替代它）；`2026-09-16-adaptation-implementation-guide.md`（前作实现记录） |

---

## 1. 背景与动机

### 1.1 现状问题

前作（`2026-09-14-xiaozhi-litecrab-agent-design.md` 与 `2026-09-16-adaptation-implementation-guide.md`）把 LiteCrab agent 部署在 PC/3516 上，作为 ESP32 与云端 ASR/LLM/TTS 之间的中转：

```
ESP32 ──WebSocket──▶ PC(LiteCrab) ──HTTPS──▶ 云端 ASR/LLM/TTS
```

存在三个问题：

1. **PC 不可省**：必须有一台 Linux 主机常开运行 `litecrab_server`；机器人无法独立工作。
2. **链路冗余**：设备→PC→云，多一段 WebSocket 帧封装/解码，延迟与故障点都加倍。
3. **与目标不符**：用户意图是 agent 直接跑在机器人上，不需要中转。

### 1.2 目标

把 agent loop（ASR→LLM 工具循环→TTS 编排）直接跑在 ESP32 固件内：

```
ESP32 ──HTTPS 直连──▶ 云端 ASR/LLM/TTS (SiliconFlow 或 火山引擎, 可配置)
  ↑
agent loop 在固件内跑: ASR→LLM(带设备 MCP 工具)→MCP tools/call(机器人控制, 已存在)→LLM→TTS
```

机器人开箱即用，无需 PC，只需联网 + 一份 API key。

### 1.3 关键洞察

现有固件架构已经具备 90% 能力，本方案的核心是"顺着既有抽象走"：

- `Protocol` 接口（`main/protocols/protocol.h`）把"设备行为"和"对话来源"解耦了。Application 只通过回调收 `OnIncomingJson(stt/llm/tts/mcp)` 与 `OnIncomingAudio(opus)`，并通过 `SendAudio/SendStartListening/SendStopListening/SendAbortSpeaking` 驱动一轮对话——它不关心对面是 WebSocket 服务、MQTT 服务，还是一个本地 agent。
- `McpServer`（`main/mcp_server.cc`）已存在，自带 `self.otto.*` 机器人工具，通过 `ParseMessage(payload)` 可被 in-process 调用，无需走网络。
- `Http` 抽象（`managed_components/78__esp-ml307/include/http.h`）已支持 HTTPS、headers、流式 Read、超时——足以实现任意云端 provider。

因此只需新增一个 `Protocol` 实现，对内满足 `Protocol` 接口、对外直接 HTTPS 调云 + 跑 agent loop，`Application.cc` 几乎不动。

### 1.4 非目标

- 不做 PLC_Diagnosis（那是能源厂区场景，机器人不需要）。设备只支持 xiaozhi-esp32-server 风格的通用对话 + 设备控制。
- 不做媒体播放（音乐/电台/新闻）。设备能力范围仅：语音对话 + 设备 MCP 工具（机器人控制）。
- 不做多用户/多设备会话持久化。设备自用，单会话，重启即重置（保留 NVS 配置）。
- 不做流式 ASR/TTS 的首版实现。首版用整段 ASR + 整段 TTS，验证可行后再优化延迟（见 §11 路线图）。
- 不删除现有 WebSocket/MQTT 协议代码。三种 `Protocol` 实现并存，按配置选择。

---

## 2. 硬约束

1. **ESP32 是 MCU，不是 Linux**：无 POSIX `exec_program`、无文件系统脚本、无 Python。LiteCrab 的 Skill 加载、`exec_program`、Skill Router 等机制无法移植。本方案不移植 LiteCrab，而是用 C++ 重写 agent loop 的最小子集。
2. **RAM 有限**：ESP32-S3 通常 512KB SRAM + 8MB PSRAM（otto-robot 板）。ASR 整段音频缓存（Opus 约 200KB/分钟）、LLM 响应（数 KB）、TTS 音频（24kHz WAV 约 4.8MB/分钟）必须放在 PSRAM，且有上限保护。
3. **HTTPS 单连接**：`Http` 对象一次一个连接，ASR/LLM/TTS 必须串行（不能并发三个 HTTPS）。这本来也符合 agent loop 的串行语义。
4. **网络不可靠**：Wi-Fi 抖动、云端限流都可能发生。每一步必须有超时 + 重试 + 失败兜底文案。
5. **设备固件协议不可协商**：`Application` 期望按 `docs/websocket_zh.md` 的消息语义收 `stt/llm/tts/mcp`——`LocalAgentProtocol` 必须产出这些消息，不能发明新契约。
6. **密钥管理**：API key 必须可配置（NVS），不硬编码进固件二进制入库。

---

## 3. 总体架构

```
┌──────────────────────────────────────────────────────────────────────────┐
│  ESP32 固件（otto-robot 板）                                              │
│                                                                            │
│  Application (main/application.cc)   ← 核心循环，几乎不改                   │
│    │ Schedule() / SetDeviceState / AudioService / Display                 │
│    │                                                                       │
│    ▼ 通过 Protocol 接口（protocol.h）                                      │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │ LocalAgentProtocol (新增)                                            │  │
│  │   - 实现 Protocol 接口，对内伪装成"服务端"                            │  │
│  │   - OpenAudioChannel: 起 agent 任务线程                              │  │
│  │   - SendAudio: 缓存 opus 帧到 ASR Provider                           │  │
│  │   - SendStartListening/SendStopListening: 驱动 ASR                   │  │
│  │   - SendAbortSpeaking: 取消在途 agent loop                           │  │
│  │   - SendMcpMessage: 设备→云（暂不用，云端 LLM 直调设备工具）           │  │
│  │   - 通过 OnIncomingJson/OnIncomingAudio 回喂 Application             │  │
│  └────────────────────────────────────────────────────────────────────┘  │
│    │                                                                       │
│    ▼ 内部组合                                                               │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐ │
│  │ AgentLoop    │  │ AsrProvider  │  │ LlmProvider  │  │ TtsProvider  │ │
│  │ (agent.cc)   │  │ (抽象+实现)   │  │ (抽象+实现)   │  │ (抽象+实现)   │ │
│  │ ASR→LLM→     │  │ opus→WAV→    │  │ HTTPS        │  │ HTTPS        │ │
│  │ 工具循环→    │  │ HTTPS→文本   │  │ /chat/comp   │  │ /audio/speech│ │
│  │ TTS 编排     │  │              │  │ (tool_calls) │  │ →WAV→opus    │ │
│  └──────────────┘  └──────────────┘  └──────────────┘  └──────────────┘ │
│    │                                                                       │
│    │ 工具调用（in-process，不走网络）                                       │
│    ▼                                                                       │
│  McpServer (main/mcp_server.cc, 已存在)                                    │
│    - ParseMessage(JSON-RPC payload) → 执行 self.otto.* / 设备工具          │
│    - 返回结果给 AgentLoop                                                  │
│                                                                            │
└──────────────────────────────────────────────────────────────────────────┘
        │ HTTPS 直连（无中转）
        ▼
┌──────────────────────────────────────────────────────────────────────────┐
│  云端 Provider（可配置切换）                                                │
│  ┌─────────────────────┐    ┌─────────────────────┐                      │
│  │ SiliconFlow         │    │ 火山引擎（豆包）     │                      │
│  │ ASR: SenseVoiceSmall│    │ ASR: 流式识别        │                      │
│  │ LLM: Qwen3.5-35B    │    │ LLM: 豆包           │                      │
│  │ TTS: CosyVoice2     │    │ TTS: 语音合成        │                      │
│  └─────────────────────┘    └─────────────────────┘                      │
└──────────────────────────────────────────────────────────────────────────┘
```

要点：

- **一个 `Protocol` 实现**替代 WebSocket 服务端：`LocalAgentProtocol` 既是"服务端"（对 Application 而言），也是"客户端"（对云端 ASR/LLM/TTS 而言）。
- **agent loop 在固件内**：ASR→LLM→工具循环→TTS 编排全部在 ESP32 上跑，不再有外置进程。
- **设备 MCP 工具直调**：`AgentLoop` 调 `McpServer::ParseMessage()` in-process，不需要 WebSocket 透传 `tools/call`——因为 agent 和 MCP server 在同一进程。
- **Application 零感知**：它仍按 `OnIncomingJson(stt)` 显示用户文本、按 `OnIncomingJson(tts sentence_start)` 显示回复、按 `OnIncomingAudio(opus)` 播放音频——完全复用现有 UI/音频路径。
- **Provider 抽象**：ASR/LLM/TTS 各一个抽象接口，SiliconFlow 与火山引擎是两个实现，按 NVS 配置选择。新增 OpenAI 兼容 provider 也容易。

---

## 4. 模块划分

### 4.1 新增文件清单

```
main/
├── protocols/
│   ├── local_agent_protocol.h        ★ 新增 ~80 行   Protocol 接口实现声明
│   └── local_agent_protocol.cc       ★ 新增 ~400 行  状态机、回调桥接、线程管理
├── agent/                             ★ 新增目录
│   ├── agent.h                       ★ 新增 ~100 行  AgentLoop 接口与配置结构
│   ├── agent.cc                      ★ 新增 ~350 行  ASR→LLM→工具循环→TTS 编排
│   ├── providers/
│   │   ├── provider.h                ★ 新增 ~120 行  AsrProvider/LlmProvider/TtsProvider 抽象
│   │   ├── siliconflow_asr.cc        ★ 新增 ~200 行  SiliconFlow ASR（SenseVoiceSmall）
│   │   ├── siliconflow_llm.cc        ★ 新增 ~250 行  SiliconFlow LLM（Qwen3.5, OpenAI 兼容）
│   │   ├── siliconflow_tts.cc        ★ 新增 ~200 行  SiliconFlow TTS（CosyVoice2）
│   │   ├── volcengine_asr.cc         ★ 新增 ~220 行  火山引擎 ASR
│   │   ├── volcengine_llm.cc         ★ 新增 ~250 行  火山引擎 LLM
│   │   └── volcengine_tts.cc         ★ 新增 ~220 行  火山引擎 TTS
│   └── conversation.h                ★ 新增 ~60 行   会话历史（messages 数组）
├── CMakeLists.txt                    ✚ 修改 ~15 行   新增源文件
└── Kconfig.projbuild                 ✚ 修改 ~30 行   新增配置项（默认 provider、超时等）
```

### 4.2 修改文件清单

```
main/application.cc                   ✚ 修改 ~10 行   InitializeProtocol() 增加 local_agent 分支
main/boards/common/board.h            （不动）          复用 GetNetwork()->CreateHttp()
main/mcp_server.*                     （不动）          复用 ParseMessage() / tools 列表
main/protocols/protocol.*             （不动）          接口契约不变
main/protocols/websocket_protocol.*   （不动）          保留，按配置共存
main/protocols/mqtt_protocol.*        （不动）          保留，按配置共存
main/Kconfig.projbuild                ✚ 修改           新增 menu "Local Agent"
main/CMakeLists.txt                   ✚ 修改           注册新源文件
sdkconfig.defaults                    ✚ 修改           默认启用 local_agent（如需）
```

### 4.3 模块职责

| 模块 | 职责 | 依赖 |
|---|---|---|
| `LocalAgentProtocol` | 实现 `Protocol` 接口；管理 agent 任务线程；把 agent 产出的事件翻译成 `OnIncomingJson/OnIncomingAudio` 回调；处理 abort | `Agent`、`Protocol`、`McpServer` |
| `Agent` | agent loop：ASR→LLM 工具循环→TTS 编排；调用 provider 与 McpServer；维护会话历史；处理取消 | `AsrProvider`、`LlmProvider`、`TtsProvider`、`McpServer`、`cJSON` |
| `AsrProvider`（抽象） | `begin/feed_audio/finish/abort` 四回调（沿用前作 LiteCrab 设计） | `Http`、Opus 解码 |
| `LlmProvider`（抽象） | `Chat(messages, tools, stream_callback) → LlmResponse`；支持 `tool_calls` | `Http`、cJSON |
| `TtsProvider`（抽象） | `Synthesize(text) → opus 帧序列` | `Http`、Opus 编码 |
| `conversation.h` | 内存会话历史（role/content 数组，固定上限，FIFO） | cJSON |

---

## 5. 数据流：一轮完整对话

```
用户唤醒/按键
    │
    ▼
Application::ToggleChatState() / WakeWordInvoke()
    │ SetDeviceState(kDeviceStateConnecting)
    ▼
LocalAgentProtocol::OpenAudioChannel()
    │ 起 agent 任务线程
    │ 发 OnAudioChannelOpened() → Application 进 listening 态
    │ 立即发 OnIncomingJson(hello) → Application 知道通道就绪
    ▼
Application::SetDeviceState(kDeviceStateListening)
    │ protocol_->SendStartListening(mode)
    ▼
LocalAgentProtocol::SendStartListening(mode)
    │ 记录 listening_mode_；asr_provider_->Begin(session_id)
    │ （不真发网络，只开缓冲）
    ▼
设备采集麦克风 → Opus 编码 → protocol_->SendAudio(packet)
    │
    ▼
LocalAgentProtocol::SendAudio(packet)
    │ asr_provider_->FeedAudio(opus_payload)
    │ （缓存 opus 帧，上限 max_audio_seconds）
    ▼
用户停说（VAD auto / 按键 manual）→ protocol_->SendStopListening()
    │
    ▼
LocalAgentProtocol::SendStopListening()
    │ 向 agent 任务线程投递 "listen_stop" 事件
    ▼
Agent 任务线程（独立 pthread）：
    ① ASR 阶段
       asr_provider_->Finish() → HTTPS 调云端 ASR → 文本
       若空文本（用户没说话）→ 静默回 IDLE，本轮结束
       否则：
         OnIncomingJson(stt{text})     → Application 显示用户话语
         OnIncomingJson(llm{emotion})  → Application 显示表情
    ② LLM 阶段（工具循环）
       conversation_.AddUser(asr_text)
       loop {
         llm_provider_->Chat(messages, mcp_tools_json, callback)
           → HTTPS POST /v1/chat/completions
           → 解析 response: {content, tool_calls[]}
         if tool_calls 为空: break
         for each tool_call:
           // in-process 调 MCP，不走网络
           McpServer::ParseMessage(JSON-RPC tools/call payload)
             → 执行 self.otto.action 等
             → 同步返回 result（McpServer 内部已 Schedule 到主任务）
           conversation_.AddToolResult(tool_call.id, result)
       }
       final_text = response.content
    ③ TTS 阶段
       sentences = SplitSentences(final_text)   // 。！？!?；;\n，≤120 字
       OnIncomingJson(tts{state:start})         → Application 进 speaking 态
       for each sentence:
         OnIncomingJson(tts{sentence_start, text})  → 屏幕显示
         opus_frames = tts_provider_->Synthesize(sentence)
           → HTTPS POST /v1/audio/speech
           → WAV → Opus 编码为 60ms 帧
         for each opus_frame:
           OnIncomingAudio(AudioStreamPacket{sample_rate=24000, payload=opus_frame})
             → AudioService 解码播放
         // 检查 aborted_ 标志，若 abort 则中断
       OnIncomingJson(tts{state:stop})          → Application 回 idle/listening
    │
    ▼
Application 按 listening_mode_ 决定回 idle 或继续 listening（auto 模式）
```

### 5.1 abort 路径

用户在 speaking 态按键/唤醒 → `Application::AbortSpeaking(reason)` → `protocol_->SendAbortSpeaking(reason)`：

- `LocalAgentProtocol::SendAbortSpeaking` 设置 `aborted_ = true`（atomic），并：
  - 若在 ASR 阶段：`asr_provider_->Abort()`（中断在途 HTTPS，关闭连接）
  - 若在 LLM 阶段：关闭 llm_provider 的 Http 连接（`Http::Close()`）
  - 若在 TTS 阶段：关闭 tts_provider 的 Http 连接；当前句播完后不再发下一句；补发 `OnIncomingJson(tts{state:stop})` 让 Application 回 idle
- agent 任务线程检查 `aborted_` 后退出当前轮次，回到等待下一轮 `listen_start` 的状态。

### 5.2 错误路径

| 阶段 | 错误 | 处理 |
|---|---|---|
| ASR | HTTPS 非 200 / 超时 / 空响应 | 重试 1 次（100ms 退避）；仍失败 → 发 `tts start` + `sentence_start("抱歉，没听清，请再说一遍")` + `tts stop`，回 idle |
| LLM | HTTPS 非 200 / 超时 / JSON 解析失败 | 重试 1 次；仍失败 → 同上道歉文案 |
| LLM | tool_calls 中某个工具执行抛异常 | `conversation_.AddToolResult(id, "工具执行失败: ...")`，让 LLM 在下一轮自行处理 |
| TTS | HTTPS 非 200 / 超时 | 跳过该句的音频，只发 `sentence_start`（文本仍显示），继续下一句；全部失败则只发文本不发音频 |
| 任意 | agent 任务线程异常退出 | `OnAudioChannelClosed()` → Application 回 idle；`SetError(message)` 触发网络错误回调 |

---

## 6. LocalAgentProtocol 详细设计

### 6.1 类接口

```cpp
// main/protocols/local_agent_protocol.h
class LocalAgentProtocol : public Protocol {
public:
    LocalAgentProtocol();
    ~LocalAgentProtocol() override;

    // Protocol 接口实现
    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

protected:
    bool SendText(const std::string& text) override;  // 仅日志，不真发

private:
    // agent 任务线程入口
    void AgentTask();
    // 把 JSON 消息回喂 Application
    void EmitJson(const std::string& type, std::function<void(cJSON*)> body_builder);
    void EmitAudio(std::vector<uint8_t> opus_payload, uint32_t timestamp);
    void EmitTtsStart();
    void EmitTtsSentence(const std::string& text);
    void EmitTtsStop();

    std::unique_ptr<Agent> agent_;
    std::atomic<bool> channel_opened_{false};
    std::atomic<bool> aborted_{false};
    // agent 任务同步：listen_start / listen_stop / abort 事件
    EventGroupHandle_t agent_events_;
    std::thread agent_thread_;
};
```

### 6.2 状态与线程模型

- **agent 任务线程**：一个独立 `std::thread`（通过 `esp_pthread_set_cfg` 配置栈大小 8KB、优先级略低于音频任务），在 `OpenAudioChannel` 时启动、`CloseAudioChannel` 时停止。
- **事件驱动**：agent 线程阻塞在 `xEventGroupWaitBits(agent_events_, LISTEN_START | LISTEN_STOP | ABORT)`。收到 `LISTEN_STOP` 时跑一轮 ASR→LLM→TTS；收到 `ABORT` 时中断当前轮。
- **回喂线程安全**：`OnIncomingJson/OnIncomingAudio` 这些回调最终会调 `Application::Schedule()`（已存在），所以 agent 线程直接调 callback 是安全的——Application 内部会串行化到主任务。但 cJSON 对象所有权要明确：callback 内部会 cJSON_Delete，所以 `EmitJson` 必须 `cJSON_Create` 一份新的、移交所有权。
- **SendAudio 线程**：由音频任务调用（高优先级），只做 `asr_provider_->FeedAudio()`（mutex 保护下的 vector push），不阻塞。

### 6.3 与 Application 的契约对齐

`Application::InitializeProtocol()`（`main/application.cc:538`）当前依据 `ota_->HasMqttConfig()` / `HasWebsocketConfig()` 选择协议。新增分支：

```cpp
void Application::InitializeProtocol() {
    // ...
    Settings protocol_settings("protocol", false);
    std::string type = protocol_settings.GetString("type", "auto");

    if (type == "local_agent") {
        protocol_ = std::make_unique<LocalAgentProtocol>();
    } else if (type == "auto") {
        // 原有逻辑：mqtt > websocket > mqtt
        if (ota_->HasMqttConfig()) {
            protocol_ = std::make_unique<MqttProtocol>();
        } else if (ota_->HasWebsocketConfig()) {
            protocol_ = std::make_unique<WebsocketProtocol>();
        } else {
            // 既无 mqtt 也无 websocket：OTA 没配，默认走 local_agent
            // 这样设备首次烧录无需 OTA 即可直接用
            protocol_ = std::make_unique<LocalAgentProtocol>();
        }
    } else if (type == "mqtt" && ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (type == "websocket" && ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "Unknown protocol type or config missing, using LocalAgentProtocol");
        protocol_ = std::make_unique<LocalAgentProtocol>();
    }
    // ... 原有回调注册不变
}
```

新增 NVS namespace `protocol` 与 key `type`，可通过配网/AP/BluFi 写入。默认值 `auto`：若 OTA 未配置（首次烧录无云端），自动走 `local_agent`，设备开箱即用。

### 6.4 hello 消息处理

`OpenAudioChannel` 成功后，`LocalAgentProtocol` 主动构造一个"服务端 hello"通过 `OnIncomingJson` 回喂 Application：

```json
{
  "type": "hello",
  "transport": "local_agent",
  "session_id": "local-<mac>",
  "audio_params": {
    "format": "opus",
    "sample_rate": 24000,
    "channels": 1,
    "frame_duration": 60
  }
}
```

- `transport: "local_agent"` 是新值，Application 只在 `websocket_protocol.cc:226` 校验 transport（那里是 `websocket`）。`LocalAgentProtocol` 自己产 hello 自己消费，不走 websocket 的 ParseServerHello，所以无冲突。
- `sample_rate: 24000`：TTS 输出 24kHz，设备 AudioCodec 会自动重采样到扬声器原生采样率（已有逻辑，见 `application.cc:572`）。
- `session_id`：用于 stt/tts 消息的回显字段（`Protocol::SendStartListening` 等会用 `session_id_`），从 MAC 派生，跨重启稳定。

---

## 7. Agent 模块详细设计

### 7.1 AgentLoop 接口

```cpp
// main/agent/agent.h
struct AgentConfig {
    std::string asr_provider;     // "siliconflow" | "volcengine"
    std::string llm_provider;
    std::string tts_provider;
    std::string asr_endpoint;
    std::string llm_endpoint;
    std::string tts_endpoint;
    std::string api_key;          // ASR/LLM/TTS 共用（SiliconFlow 模式）
    std::string llm_model;        // e.g. "Qwen/Qwen3.5-35B-A3B"
    std::string asr_model;        // e.g. "FunAudioLLM/SenseVoiceSmall"
    std::string tts_model;        // e.g. "FunAudioLLM/CosyVoice2-0.5B"
    std::string tts_voice;        // e.g. "alex"
    int asr_timeout_ms = 30000;
    int llm_timeout_ms = 60000;
    int tts_timeout_ms = 30000;
    int max_audio_seconds = 60;
    int max_tool_iterations = 5;
    int conversation_history_limit = 10;  // 保留最近 N 轮
};

class Agent {
public:
    using EmitJsonFn = std::function<void(const std::string& type, std::function<void(cJSON*)> builder)>;
    using EmitAudioFn = std::function<void(std::vector<uint8_t> opus, uint32_t timestamp)>;
    using IsAbortedFn = std::function<bool()>;

    Agent(AgentConfig cfg, EmitJsonFn emit_json, EmitAudioFn emit_audio, IsAbortedFn is_aborted);
    ~Agent();

    // 供 LocalAgentProtocol 调用
    void OnListenStart();
    void OnListenStop();     // 触发一轮 ASR→LLM→TTS
    void OnAudioFrame(const uint8_t* opus, size_t len);
    void OnAbort();

private:
    void RunTurn();
    std::string DoAsr();
    std::string DoLlmToolLoop(const std::string& user_text);
    void DoTts(const std::string& text);
    std::vector<uint8_t> SynthesizeSentence(const std::string& text);

    AgentConfig cfg_;
    EmitJsonFn emit_json_;
    EmitAudioFn emit_audio_;
    IsAbortedFn is_aborted_;
    std::unique_ptr<AsrProvider> asr_;
    std::unique_ptr<LlmProvider> llm_;
    std::unique_ptr<TtsProvider> tts_;
    Conversation conversation_;
};
```

### 7.2 工具循环

`DoLlmToolLoop` 伪代码：

```cpp
std::string Agent::DoLlmToolLoop(const std::string& user_text) {
    conversation_.AddUser(user_text);
    std::string tools_json = BuildToolsJson();  // 见 §7.3
    for (int iter = 0; iter < cfg_.max_tool_iterations; ++iter) {
        if (is_aborted_()) return "";
        LlmResponse resp = llm_->Chat(conversation_.Messages(), tools_json);
        if (!resp.tool_calls.empty()) {
            conversation_.AddAssistant(resp.content, resp.tool_calls);
            for (const auto& tc : resp.tool_calls) {
                if (is_aborted_()) return "";
                std::string result = InvokeMcpTool(tc.name, tc.arguments);
                conversation_.AddToolResult(tc.id, result);
            }
            continue;
        }
        conversation_.AddAssistant(resp.content);
        return resp.content;
    }
    return "抱歉，处理步骤过多，请简化问题。";
}
```

### 7.3 设备 MCP 工具注入 LLM

LLM 需要看到设备工具（`self.otto.*`、`self.get_device_status` 等）的 JSON schema 才能调用。`McpServer` 内部维护 `tools_` 列表，每个 `McpTool` 有 `to_json()` 方法产出 OpenAI 工具格式（name/description/inputSchema）。

```cpp
std::string Agent::BuildToolsJson() {
    // McpServer 不直接暴露 tools_ 列表，需要新增一个公有方法
    // 或通过 McpServer::ParseMessage(tools/list) 获取
    // 建议在 mcp_server.h 新增：
    //   std::string GetToolsListJson() const;
    // 返回 [{name, description, inputSchema}, ...] 的 JSON 数组字符串
    return McpServer::GetInstance().GetToolsListJson();
}
```

**对 `mcp_server.h` 的最小改动**：新增一个 `GetToolsListJson()` 公有方法（不改既有 `ParseMessage` 路径）。约 10 行。

### 7.4 工具调用（in-process）

`InvokeMcpTool` 构造 JSON-RPC `tools/call` payload，调 `McpServer::ParseMessage`：

```cpp
std::string Agent::InvokeMcpTool(const std::string& name, const std::string& arguments) {
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "jsonrpc", "2.0");
    cJSON_AddStringToObject(payload, "method", "tools/call");
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", name.c_str());
    cJSON* args = cJSON_Parse(arguments.c_str());
    cJSON_AddItemToObject(params, "arguments", args);
    cJSON_AddItemToObject(payload, "params", params);
    cJSON_AddNumberToObject(payload, "id", next_tool_id_++);

    // McpServer::ParseMessage 是同步的：执行工具并产出 result
    // 它内部会调 Application::Schedule() 把 UI 更新串行化到主任务
    // 但工具执行结果（ReturnValue）是同步返回的
    std::string result = McpServer::GetInstance().ParseMessage(payload);
    cJSON_Delete(payload);
    return ExtractToolResultText(result);  // 从 JSON-RPC result.content[0].text 取文本
}
```

**重要**：`McpServer::ParseMessage` 当前的回复路径是 `ReplyResult` / `ReplyError`，它通过 `protocol_->SendMcpMessage()` 把结果发回网络。但本场景下 agent 在 in-process 直接拿结果，不需要走网络。`McpServer` 需要一个小改动：增加一个"直接返回结果"的调用模式（不经过 `SendMcpMessage`）。建议新增方法：

```cpp
// mcp_server.h 新增
std::string McpServer::CallToolDirectly(const std::string& name, const cJSON* arguments);
// 直接执行工具并返回 JSON-RPC result 字符串，不走 protocol_->SendMcpMessage
```

约 20 行改动，不动既有 `ParseMessage` 路径。

### 7.5 会话历史

```cpp
// main/agent/conversation.h
struct ConversationMessage {
    std::string role;        // "user" | "assistant" | "tool"
    std::string content;
    std::string tool_call_id;   // role=="tool" 时
    std::string tool_calls;     // role=="assistant" 且有工具调用时（JSON 字符串）
};

class Conversation {
public:
    void AddUser(const std::string& text);
    void AddAssistant(const std::string& content, const std::vector<ToolCall>& calls = {});
    void AddToolResult(const std::string& tool_call_id, const std::string& result);
    std::string ToJson() const;  // 序列化为 OpenAI messages 数组
    void Clear();
private:
    std::vector<ConversationMessage> msgs_;
    int limit_;  // 超过则 FIFO 丢弃最旧 user/assistant 对
};
```

上限 `conversation_history_limit = 10` 轮（约 20 条消息），防 RAM 膨胀。系统 prompt（见 §7.6）不进 FIFO，每轮固定前置。

### 7.6 系统提示词

固定系统 prompt（作为常量字符串嵌入 flash，不占 RAM）：

```
你是一个运行在机器人上的语音助手。你可以通过工具控制机器人的动作（走路、转身等）和设备状态（音量、亮度等）。
当用户要求执行动作时，先调用对应工具，再根据工具结果回答用户。
回答简洁，使用中文，每句不超过 60 字。不要透露你的内部实现。
```

每轮 LLM 调用前，messages 结构：
```
[
  {role: "system", content: SYSTEM_PROMPT},
  ...conversation_.Messages() (FIFO 历史),
  // 当前用户输入已在 conversation_ 末尾
]
```

### 7.7 分句器

沿用前作 LiteCrab 的规则（已验证）：

- 按 `。！？!?；;\n` 切分
- 单句上限 120 字符，超长硬切（UTF-8 不拆半字符）
- 空句跳过

实现约 40 行 C++，放 `agent.cc` 内部静态函数。

---

## 8. Provider 抽象与实现

### 8.1 抽象接口

```cpp
// main/agent/providers/provider.h
class AsrProvider {
public:
    virtual ~AsrProvider() = default;
    virtual void Begin(const std::string& session_id) = 0;
    virtual void FeedAudio(const uint8_t* opus, size_t len) = 0;
    // 返回 0=成功（text 可能为空，表示用户没说话）
    //       1=回落桩文本（cfg.fallback_text）
    //      -1=硬失败
    virtual int Finish(std::string& text) = 0;
    virtual void Abort() = 0;
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;  // JSON 字符串
};

struct LlmResponse {
    std::string content;
    std::vector<ToolCall> tool_calls;
};

class LlmProvider {
public:
    virtual ~LlmProvider() = default;
    virtual LlmResponse Chat(const std::string& messages_json,
                             const std::string& tools_json) = 0;
    virtual void Abort() = 0;
};

class TtsProvider {
public:
    virtual ~TtsProvider() = default;
    // 返回 opus 帧序列（每帧 60ms，长度前缀或裸帧由实现决定）
    virtual std::vector<std::vector<uint8_t>> Synthesize(const std::string& text) = 0;
    virtual void Abort() = 0;
};
```

### 8.2 SiliconFlow 实现

#### AsrProvider (`siliconflow_asr.cc`)

- `Begin`: 清空 opus 帧缓冲（`std::vector<uint8_t>`，PSRAM 分配）
- `FeedAudio`: append opus 帧到缓冲（mutex 保护，上限 `max_audio_seconds * 200KB/60s`）
- `Finish`:
  1. libopus 逐帧 `opus_decode` → PCM 16k/16bit/mono（复用 `audio_codec` 已有的 opus decoder？或新建一个）
  2. 封 44 字节 WAV 头
  3. `Http` POST `https://api.siliconflow.cn/v1/audio/transcriptions`
     - multipart/form-data 构造（boundary + Content-Disposition + file 字段）
     - header: `Authorization: Bearer <api_key>`
     - body: `model=FunAudioLLM/SenseVoiceSmall` + `file=audio.wav`
     - 超时 `asr_timeout_ms`
  4. 解析响应 `{"text": "..."}` 取文本
  5. 失败重试 1 次（100ms 退避）；仍失败返回 1（回落）或 -1（硬失败）
- `Abort`: 关闭 `Http` 连接

**libopus 依赖**：固件 `audio_codec` 已链接 opus（用于麦克风编码和 TTS 解码）。直接复用 `opus_decode` 函数，不新增依赖。需要确认 `main/audio/audio_codec.h` 是否暴露了 decoder 接口；若未暴露，新增一个 `OpusDecoder` 工具类（~80 行）。

#### LlmProvider (`siliconflow_llm.cc`)

- `Chat`:
  1. 构造 OpenAI 兼容请求体：
     ```json
     {
       "model": "Qwen/Qwen3.5-35B-A3B",
       "messages": [...],
       "tools": [...],
       "tool_choice": "auto",
       "stream": false
     }
     ```
  2. `Http` POST `https://api.siliconflow.cn/v1/chat/completions`
     - header: `Authorization: Bearer <api_key>`, `Content-Type: application/json`
     - 超时 `llm_timeout_ms`
  3. 解析响应：
     - `choices[0].message.content` → `LlmResponse.content`
     - `choices[0].message.tool_calls[]` → `LlmResponse.tool_calls`（每个含 `id/function.name/function.arguments`）
  4. 失败重试 1 次
- `Abort`: 关闭 `Http` 连接

**首版不做流式**（`stream: false`）。流式优化见 §11。

#### TtsProvider (`siliconflow_tts.cc`)

- `Synthesize`:
  1. `Http` POST `https://api.siliconflow.cn/v1/audio/speech`
     - body: `{"model":"FunAudioLLM/CosyVoice2-0.5B","input":"<text>","voice":"alex"}`
     - 超时 `tts_timeout_ms`
  2. 响应是 WAV 音频（24kHz/mono/16bit，chunked 传输编码，需自己解码 chunked）
  3. WAV → PCM → Opus 编码为 60ms 帧（复用 `audio_codec` 的 `opus_encode`）
  4. 返回 opus 帧数组
- `Abort`: 关闭 `Http` 连接

### 8.3 火山引擎实现

火山引擎的 ASR/LLM/TTS 接口与 OpenAI 兼容性部分不同，需要分别适配：

- **ASR**：火山引擎 ASR 主要是流式 WebSocket 协议（非 OpenAI 兼容 HTTP）。首版可暂用火山引擎的 HTTP 录音文件识别接口（`/api/v1/asr`），或退而用 SiliconFlow ASR（即使 LLM/TTS 走火山）。
- **LLM**：豆包 API 是 OpenAI 兼容（`/api/v3/chat/completions`），直接复用 `siliconflow_llm.cc` 的逻辑，只改 endpoint 和 header（`Authorization: Bearer <volc_key>` 或火山特有的 `X-Api-App-Id` + `X-Api-Sign`）。
- **TTS**：火山 TTS 是 HTTP 二进制响应（WAV 或 MP3），类似 SiliconFlow，复用 `siliconflow_tts.cc` 框架。

**建议**：首版优先实现 SiliconFlow 三件套（已验证可用），火山引擎作为第二阶段补充。配置项预留 `volcengine_*` 字段，实现先 stub。

### 8.4 chunked 传输编码

SiliconFlow TTS 响应是 chunked transfer-encoding（前作已踩坑）。`Http::Read()` 返回的是原始 chunked 数据，需要在 provider 内部解码：

- 解析 chunk 长度行（十六进制 + CRLF）
- 拼接 chunk payload
- 遇 `0\r\n\r\n` 结束

约 60 行 C++，放 `provider.h` 的公用工具函数 `ReadChunkedBody(Http* http)`。

---

## 9. 配置设计

### 9.1 NVS 配置（运行时可改）

新增 NVS namespace `agent`：

| key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `provider` | string | `siliconflow` | `siliconflow` / `volcengine` |
| `api_key` | string | （空） | SiliconFlow 模式下 ASR/LLM/TTS 共用 |
| `volc_asr_key` | string | （空） | 火山 ASR key |
| `volc_llm_key` | string | （空） | 火山 LLM key |
| `volc_tts_key` | string | （空） | 火山 TTS key |
| `llm_model` | string | `Qwen/Qwen3.5-35B-A3B` | LLM 模型名 |
| `asr_model` | string | `FunAudioLLM/SenseVoiceSmall` | ASR 模型名 |
| `tts_model` | string | `FunAudioLLM/CosyVoice2-0.5B` | TTS 模型名 |
| `tts_voice` | string | `alex` | TTS 音色 |
| `llm_endpoint` | string | `https://api.siliconflow.cn/v1/chat/completions` | LLM 端点（可覆盖） |
| `asr_endpoint` | string | `https://api.siliconflow.cn/v1/audio/transcriptions` | ASR 端点 |
| `tts_endpoint` | string | `https://api.siliconflow.cn/v1/audio/speech` | TTS 端点 |
| `system_prompt` | string | （内置默认） | 自定义系统提示词（可选） |

NVS namespace `protocol`：

| key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `type` | string | `auto` | `auto` / `local_agent` / `websocket` / `mqtt` |

### 9.2 Kconfig（编译期默认值）

`main/Kconfig.projbuild` 新增 menu：

```
menu "Local Agent"
config LOCAL_AGENT_DEFAULT_PROVIDER
    string "Default cloud provider"
    default "siliconflow"
    help
        siliconflow or volcengine

config LOCAL_AGENT_DEFAULT_LLM_MODEL
    string "Default LLM model"
    default "Qwen/Qwen3.5-35B-A3B"

config LOCAL_AGENT_DEFAULT_ASR_MODEL
    string "Default ASR model"
    default "FunAudioLLM/SenseVoiceSmall"

config LOCAL_AGENT_DEFAULT_TTS_MODEL
    string "Default TTS model"
    default "FunAudioLLM/CosyVoice2-0.5B"

config LOCAL_AGENT_DEFAULT_TTS_VOICE
    string "Default TTS voice"
    default "alex"

config LOCAL_AGENT_ASR_TIMEOUT_MS
    int "ASR timeout (ms)"
    default 30000

config LOCAL_AGENT_LLM_TIMEOUT_MS
    int "LLM timeout (ms)"
    default 60000

config LOCAL_AGENT_TTS_TIMEOUT_MS
    int "TTS timeout (ms)"
    default 30000

config LOCAL_AGENT_MAX_AUDIO_SECONDS
    int "Max audio recording seconds"
    default 60

config LOCAL_AGENT_MAX_TOOL_ITERATIONS
    int "Max LLM tool iterations per turn"
    default 5

config LOCAL_AGENT_CONVERSATION_HISTORY_LIMIT
    int "Conversation history rounds to keep"
    default 10
endmenu
```

API key 不进 Kconfig（编译期不存密钥），仅 NVS 运行时配置。

### 9.3 配置写入方式

设备首次使用需写入 `agent/api_key`。可选方式：

1. **配网 AP**：设备进入配网模式时，Web 表单同时收 SSID/密码/API key（需小改配网页面）。
2. **BluFi**：通过蓝牙配网协议扩展字段写入。
3. **手动 NVS 工具**：esptool 写 NVS 分区（开发期）。
4. **语音引导**：首次启动若检测到 `api_key` 为空，TTS 播报"请通过配网页面设置 API key"。

首版推荐方式 1（配网 AP 表单加字段），最简单。

---

## 10. 对既有代码的改动清单

### 10.1 `main/application.cc`（约 +10 行）

`InitializeProtocol()` 增加 `local_agent` 分支（见 §6.3 伪代码）。其余不动。

### 10.2 `main/mcp_server.h` / `mcp_server.cc`（约 +30 行）

新增两个公有方法：

```cpp
// mcp_server.h
public:
    // 返回所有工具的 OpenAI tools 格式 JSON 数组字符串
    std::string GetToolsListJson() const;
    // 直接执行工具，返回 JSON-RPC result 字符串（不走 protocol_->SendMcpMessage）
    std::string CallToolDirectly(const std::string& name, const cJSON* arguments);
```

实现复用既有 `DoToolCall` 内部逻辑，只是不调 `ReplyResult`（ReplyResult 会走 protocol 发网络），而是直接返回 result 字符串。

### 10.3 `main/protocols/local_agent_protocol.h` / `.cc`（新增约 500 行）

见 §6 设计。

### 10.4 `main/agent/*`（新增约 1500 行）

见 §7、§8 设计。

### 10.5 `main/CMakeLists.txt`（约 +15 行）

注册新源文件：

```cmake
if(CONFIG_LOCAL_AGENT_DEFAULT_PROVIDER)
    target_sources(${COMPONENT_LIB} PRIVATE
        protocols/local_agent_protocol.cc
        agent/agent.cc
        agent/conversation.cc
        agent/providers/provider_common.cc
        agent/providers/siliconflow_asr.cc
        agent/providers/siliconflow_llm.cc
        agent/providers/siliconflow_tts.cc
        # agent/providers/volcengine_asr.cc  # 第二阶段
        # agent/providers/volcengine_llm.cc
        # agent/providers/volcengine_tts.cc
    )
endif()
```

### 10.6 `main/Kconfig.projbuild`（约 +30 行）

见 §9.2。

### 10.7 `sdkconfig.defaults`（可选）

若希望默认启用 local_agent：

```
CONFIG_PROTOCOL_TYPE="local_agent"
```

否则保持 `auto`，依赖 NVS 配置。

---

## 11. 测试计划

### 11.1 单元测试（host 侧，需 mock）

ESP-IDF 支持 host 侧测试（`idf.py create-component --test`）。但本方案大量依赖 ESP-IDF API（Http、cJSON、esp_pthread），纯 host 单测成本高。建议：

- **分句器**：纯函数，host 单测（中英文标点、超长硬切、UTF-8 安全、空句跳过）
- **chunked 解码器**：纯函数，host 单测（RFC 7230 向量）
- **会话历史 FIFO**：纯逻辑，host 单测
- **OpenAI 请求体构造**：纯 JSON 构造，host 单测
- **LLM 响应解析**（含 tool_calls）：纯 JSON 解析，host 单测

这些放 `main/test/` 目录，`idf.py test` 运行。

### 11.2 集成测试（设备侧 + mock server）

在 PC 上起 mock HTTPS server（Python `http.server` + SSL），模拟 SiliconFlow 三个端点：

- mock ASR：校验 multipart body，返回 `{"text":"向前走三步"}`
- mock LLM：收到"向前走三步" → 返回 tool_call `self.otto.action({action:walk, steps:3})` → 收到工具结果 → 返回 final text "好的，我向前走了三步"
- mock TTS：返回固定 WAV（正弦波）

设备连 PC 跑完整 agent loop，断言：

1. 设备屏幕显示用户话语"向前走三步"
2. 机器人执行 walk 动作
3. 设备屏幕显示回复"好的，我向前走了三步"
4. 扬声器播放 TTS 音频

这个测试需要真机（机器人执行动作无法 mock）。开发期可先 mock `McpServer::CallToolDirectly` 返回固定结果，验证 agent loop 逻辑；再上真机验证 MCP 工具执行。

### 11.3 真机验证

1. 烧录固件（`python scripts/build.py otto-robot`）
2. 配网 AP 写入 `agent/api_key`
3. 唤醒 → 说"你好" → 断言屏幕显示用户话语 + 回复 + 扬声器播放
4. 唤醒 → 说"向前走三步" → 断言机器人执行 walk + 回复 + 音频
5. 唤醒 → 说"把音量调到 50" → 断言音量变化 + 回复
6. abort 测试：说话过程中按键打断 → 断言停止 TTS、回 idle
7. 网络错误：关 Wi-Fi → 唤醒说话 → 断言道歉文案
8. 长时间稳定性：连续 20 轮对话，观察 RAM 是否泄漏（`esp_get_free_heap_size` 日志）

---

## 12. 错误处理与超时汇总

| 场景 | 行为 |
|---|---|
| ASR HTTPS 非 200 / 超时 | 重试 1 次（100ms）；仍失败 → 道歉 TTS "抱歉，没听清" |
| ASR 返回空文本 | 静默回 idle，不调 LLM（省一次调用） |
| LLM HTTPS 非 200 / 超时 | 重试 1 次；仍失败 → 道歉 TTS |
| LLM tool_calls 格式错误 | `AddToolResult(id, "工具调用格式错误")`，让 LLM 自行处理 |
| MCP 工具执行抛异常 | `AddToolResult(id, "工具执行失败: <msg>")` |
| 工具循环超 5 次 | 强制返回"处理步骤过多"文案 |
| TTS HTTPS 失败 | 跳过该句音频，只显示文本 |
| 单轮音频超 60s | 截断缓冲，仍走 ASR |
| `api_key` 未配置 | 启动时 TTS 播报"请配置 API key"；OpenAudioChannel 直接失败 |
| agent 任务线程崩溃 | `OnAudioChannelClosed` + `SetError` |
| abort | 中断当前 HTTPS，补发 `tts stop` |

---

## 13. 资源评估

### 13.1 RAM（PSRAM 为主）

| 项 | 大小 | 说明 |
|---|---|---|
| ASR opus 缓冲 | ~200KB/分钟 × 上限 60s = 200KB | PSRAM |
| ASR PCM 解码缓冲 | 16k×16bit×60s = 1.9MB | PSRAM，临时 |
| LLM 请求/响应 | ~10KB | 栈/堆 |
| 会话历史 | ~5KB（10 轮） | 堆 |
| TTS WAV 缓冲 | 24k×16bit×60s = 2.8MB（单句通常 < 5s，~280KB） | PSRAM |
| agent 任务栈 | 8KB | 内部 SRAM |
| Http 对象 | ~4KB × 1（串行复用） | 堆 |

**峰值 PSRAM 占用约 2MB**（ASR 解码 + TTS 缓冲不会同时，因为串行）。otto-robot 8MB PSRAM 充足。

### 13.2 Flash

新增代码约 2000 行 C++，编译后约 200-300KB。otto-robot 16MB Flash 充足。

### 13.3 CPU

- ASR 期间：libopus 解码（~5% CPU）+ HTTPS 传输
- LLM 期间：HTTPS 传输 + JSON 解析
- TTS 期间：libopus 编码（~10% CPU）+ HTTPS 传输

ESP32-S3 双核 240MHz，agent 任务线程与音频任务（高优先级）分核运行，不冲突。

---

## 14. 迁移步骤（建议实施顺序）

| 步骤 | 内容 | 验证 |
|---|---|---|
| 1 | 新增 `main/agent/` 目录骨架：`provider.h`（抽象接口）、`conversation.h`、`agent.h`（空实现） | 编译通过 |
| 2 | 实现 `siliconflow_llm.cc` + host 单测（mock Http） | 单测 PASS |
| 3 | 实现 `siliconflow_asr.cc`（含 opus 解码、WAV 封装、multipart） + host 单测 | 单测 PASS |
| 4 | 实现 `siliconflow_tts.cc`（含 chunked 解码、opus 编码） + host 单测 | 单测 PASS |
| 5 | 实现 `agent.cc`（ASR→LLM→工具循环→TTS 编排），mock `McpServer::CallToolDirectly` | host 集成测试 |
| 6 | 改 `mcp_server.h/cc`：加 `GetToolsListJson` + `CallToolDirectly` | 既有 MCP 测试不回归 |
| 7 | 实现 `local_agent_protocol.cc` + 改 `application.cc::InitializeProtocol` | 编译通过 |
| 8 | 真机：配网写 key → 唤醒说话 → 验证 ASR/LLM/TTS 全链路 | §11.3 场景 1-3 |
| 9 | 真机：验证 MCP 工具调用（机器人动作） | §11.3 场景 4-5 |
| 10 | 真机：abort、网络错误、稳定性 | §11.3 场景 6-8 |
| 11 | （第二阶段）实现 `volcengine_*` provider | 切换配置可跑 |
| 12 | （第二阶段）流式 ASR/TTS 降延迟 | 见 §15 |

---

## 15. 后续优化路线图（非首版）

1. **流式 ASR**：SiliconFlow ASR 当前是整段上传+识别（1-2s 延迟）。火山引擎流式 ASR（WebSocket）可在说话同时识别，延迟降到 200ms。需要新增 `StreamingAsrProvider`。
2. **流式 TTS**：SiliconFlow TTS 支持 stream 模式，边合成边下发 opus 帧，首音延迟从整句降到 300ms。
3. **流式 LLM**：OpenAI 兼容 `stream: true`，边生成边返回 token，TTS 可在第一个句号后立即合成。需要 SSE 解析。
4. **火山引擎 provider**：补齐 `volcengine_*.cc`。
5. **会话持久化**：当前会话历史在 RAM，重启丢失。可写 NVS 或 SPIFFS 文件。
6. **多 provider 混合**：ASR 用火山流式、LLM 用 SiliconFlow、TTS 用火山。配置项已预留。
7. **prompt cache 优化**：系统 prompt + 工具 schema 较长，利用 SiliconFlow 的 prompt cache（前 1k token 缓存）降成本和延迟。把工具 schema 放在 messages 前部并保持稳定。

---

## 16. 风险与开放问题

| # | 风险/问题 | 影响 | 缓解 |
|---|---|---|---|
| R1 | libopus decoder 是否已被 `audio_codec` 暴露可复用 | 若未暴露需新写 decoder 包装（~80 行） | 步骤 3 前先读 `audio_codec.h` 确认 |
| R2 | SiliconFlow 免费档限流（ASR 排队 20-55s） | 交互延迟高 | 超时设 30s + 重试；第二阶段切火山流式 ASR |
| R3 | ESP32 HTTPS TLS 握手耗时（~1-2s） | 每轮对话多 3 次握手 | Http Keep-Alive（若 provider 支持）；或连接池 |
| R4 | chunked 解码边角 bug | TTS 音频截断 | host 单测覆盖 RFC 7230 向量 |
| R5 | agent 任务线程与音频任务优先级冲突 | 音频卡顿 | `esp_pthread_set_cfg` 设优先级低于音频；实测调优 |
| R6 | RAM 碎片化（长稳压测） | 崩溃 | 20 轮稳定性测试；必要时用 `heap_caps_print_heap_info` 排查 |
| R7 | `McpServer::CallToolDirectly` 改动影响既有 MCP 路径 | WebSocket 协议下 MCP 失效 | 新增方法不动 `ParseMessage`；既有测试不回归 |
| R8 | 首次配置 API key 的 UX | 用户不知道怎么配 | 配网 AP 表单加字段 + TTS 引导播报 |
| O1 | 是否需要支持 OpenAI 官方端点 | 海外用户 | 抽象接口已对齐，`openai_*.cc` 实现成本低，按需补 |
| O2 | 系统 prompt 是否应该可热更新 | 迭代 convenience | NVS `system_prompt` 字段已预留，空则用默认 |
| O3 | 设备如何知道"正在处理"给用户反馈 | LLM 长耗时用户无感 | ASR 完成后立即显示 stt；LLM 期间可显示"思考中..."动画 |

---

## 17. 与前作（PC 中转方案）的关系

本方案**替代**前作的 PC 中转架构，不是叠加：

- `server/` 目录（LiteCrab + xiaozhi_ws 网关）在本方案下**不再需要**。可保留代码作为参考，但运行时不依赖。
- 设备固件的 `sdkconfig.defaults` 中 `CONFIG_OTA_URL` 可恢复默认（`https://api.tenclass.net/xiaozhi/ota/`）或留空——因为 `protocol/type=local_agent` 时不走 OTA 协议选择路径。
- 前作的 `server/tests/`（fake_device 等）的测试逻辑可借鉴为本方案的 host 集成测试。

**迁移路径**：已有 otto-robot 设备从 PC 中转切到本地 agent，只需：
1. 刷带 `local_agent` 代码的新固件
2. 配网写入 `agent/api_key`
3. 重启

无需改 PC 侧任何东西（PC 侧 LiteCrab 可直接关停）。

---

## 18. 总结

本方案的核心价值：

1. **去中转**：agent loop 跑在 ESP32 上，设备直接 HTTPS 调云端，PC 完全退出链路。
2. **最小改动**：顺着既有 `Protocol` 抽象走，`Application.cc` 仅改 10 行，音频/唤醒/显示/MCP 全复用。
3. **可配置**：SiliconFlow 与火山引擎可切换，API key 走 NVS，不硬编码。
4. **可共存**：不删除 WebSocket/MQTT 协议，三种 `Protocol` 按配置选择，未来需要连官方云时切回即可。
5. **可演进**：Provider 抽象让新增 provider 成本低；流式优化有清晰路线（§15）。

风险可控：核心逻辑（agent loop、provider）可在 host 侧单测；真机验证聚焦音频/MCP/abort/稳定性四个维度。
