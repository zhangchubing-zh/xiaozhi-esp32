# xiaozhi-esp32 × LiteCrab 本地 Agent 集成设计

| 项目 | 内容 |
|---|---|
| 日期 | 2026-09-14（2026-09-15 修订 3） |
| 状态 | **阶段 1+2(TTS)+3(MCP设备工具) 已实现并通过端到端验证**（mock e2e 7/7 + 真实 SiliconFlow ASR/LLM/TTS 全链路，LLM 成功自主调用 otto 机器人指令） |
| 范围 | 将 LiteCrab agent 部署为 xiaozhi-esp32 设备的本地服务端，替代 xiaozhi.me 云端；agent 会话前接入 SiliconFlow ASR（SenseVoiceSmall）将语音转写为文本；回复以文本+语音（CosyVoice2 TTS）下发；agent 可调用设备端 MCP 工具驱动机器人 |
| 涉及仓库 | `zhangchubing-zh/xiaozhi-esp32`（本仓库，固件 + `server/` 服务端唯一真源）；上游 `LiteCrab_0829` 冻结不改 |

> 2026-09-14 修订 1：ASR 由阶段 2 提前至阶段 1——agent 会话前调用 SiliconFlow 转写接口（SenseVoiceSmall，免费）将语音转为文本；桩文本降级为故障回落与联调模式。
>
> 2026-09-14 修订 2：**部署目标由 5091 板改为 3516 板**；`server/litecrab` 反转为唯一真源（上游 `LiteCrab_0829` 不再直接修改，对齐 §6 备选项）；LLM/ASR key 按项目要求直接写入配置入库。实现落地于 `server/litecrab/src/gateway/`（ws_codec / asr_sf / xiaozhi_ws）。
>
> 2026-09-15 修订 3：**TTS 与 MCP 设备工具桥提前实现**（原阶段 2/3 内容）——
> ① `tts_sf.c`：回复经 SiliconFlow CosyVoice2（24kHz）合成 + libopus 编码为
> 60ms Opus 帧随 `sentence_start` 后二进制下发，server hello 声明
> `audio_params.sample_rate=24000`，设备端自动重采样（B 组调研确认）；
> ② MCP 桥：设备连接后网关自动 `initialize`→`tools/list`（支持 8000 字节分页），
> 设备工具（如 `self.otto.action`）经 `CrabRegistryRegister`+`AgentLoopRefreshTools`
> （kernel.c 新增，运行时重渲染 tools JSON）注册给 LLM；LLM 调用时经 WS
> `tools/call` JSON-RPC 下发设备执行并等待结果（20s 超时，发送/等待全程线程安全）。
> 用户语音必经 ASR；LLM 内部循环与工具结果不经过 ASR。

---

## 1. 背景与目标

xiaozhi-esp32 是 ESP32 语音对话终端：设备通过 WebSocket 连接服务端，上传 Opus 音频，服务端（xiaozhi.me 或自建）完成 ASR + LLM + TTS，回传识别文本、回复文本和合成音频，并通过 MCP（JSON-RPC over WebSocket）控制设备。

目标：**用自研 LiteCrab agent 替代云端服务端**，跑在 5091 ARM 板上，形成"设备 → 局域网 → 板端 agent → 云端 LLM"的链路。agent 的能源领域 Skill（PLC_Diagnosis、告警诊断等）直接服务语音入口。

分阶段目标：

- **阶段 1（本文档重点）**：跑通链路，**含真实 ASR**——设备照常采集上传 Opus 音频，网关缓存音频，在采集结束时调用 **SiliconFlow 语音识别 API（SenseVoiceSmall，免费）** 将语音转写为文本，作为用户话语提交 agent（ASR 失败或未配置时回落固定桩文本）；agent 回复以**纯文本**下发到设备屏幕显示（不做 TTS 音频）。
- **阶段 2**：TTS 下行（HTTP 合成 + 音频帧）；可选流式 ASR 进一步降延迟。
- **阶段 3**：MCP 双向打通（agent 调用设备端工具、ASR/TTS 可选 MCP 化）。

### 1.1 非目标（阶段 1 明确不做）

- 不做 TTS（回复纯文本屏显）；不做流式 ASR（`listen stop` 后整段转写）。音频转写后即弃，不入库、不留存。
- 不实现 MCP `initialize` / `tools/call` 的桥接（设备侧 MCP 能力闲置，无副作用）。
- 不做 wss/TLS（局域网明文 ws://）。
- 不修改 ESP32 固件代码（仅重刷带自定义 sdkconfig 的固件，见 §7.7）。

---

## 2. 现状事实（代码依据）

### 2.1 xiaozhi-esp32 侧（本仓库）

设备端配置与连接链路（已核实）：

1. 设备启动联网后进入激活态，`Application::ActivationTask()`（`main/application.cc:354`）调用 `Ota::CheckVersion()`。
2. `CheckVersion()` 向 `CONFIG_OTA_URL`（编译期 Kconfig 默认值，可被 NVS `wifi/ota_url` 覆盖，见 `main/ota.cc:46-52`）发起 **HTTP POST**（携带系统信息 JSON），期望 200 + JSON。
3. 响应中的 `websocket` 段被逐键写入 NVS `Settings("websocket")`（`url`、`token`、`version`），见 `main/ota.cc:168-186`。
4. `Application::InitializeProtocol()`（`main/application.cc:527-534`）：响应**无 `mqtt` 段**时选择 `WebsocketProtocol`。
5. 唤醒/按键触发 `WebsocketProtocol::OpenAudioChannel()`（`main/protocols/websocket_protocol.cc:79`）：从 NVS 读 `url`/`token`/`version`，握手时携带请求头 `Authorization: Bearer <token>`、`Protocol-Version`、`Device-Id`（MAC）、`Client-Id`（UUID）。
6. 连接后设备发 `hello`（含 `features.mcp`、`audio_params`），等待服务端 `hello`（10 秒超时）；随后进入监听，发 `listen start`，上传二进制 Opus 帧；停止采集发 `listen stop`。
7. 服务端下发 `stt`/`llm`/`tts`（start / sentence_start / stop）等 JSON 消息；`tts start` 使设备进入说话态并停止上行，`sentence_start` 的 `text` 显示为聊天气泡，`tts stop` 结束回到 idle（auto 模式回 listening）。

协议细节见 `docs/websocket.md`（本仓库）。

### 2.2 LiteCrab 侧（LiteCrab_0829 仓库）

- C11 Agent Runtime，POSIX 线程，单二进制 `litecrab_server`；x86 WSL 开发、5091（armv7hf）交叉编译全静态部署。
- 对外入口是**换行分隔的 TCP JSON 网关** `src/gateway/tcp_line.c`（`{"userId","sessionId","content"}` → 同步回复）。
- 内部统一入口为消息总线 + Ingress（`include/litecrab/hub.h`）：
  - `IngressSubmit(content, len, IngressOptions, IngressResult*)`（hub.h:74）——提交 `LITE_MSG_CHAT`，`LITE_REPLY_SYNC` 模式下携带 `requestId` 同步取回复；
  - `MessageBusTimedPopOutboundByRequestId()`（hub.h:72）——按 `requestId` 限时等待回复；
  - `RequestCancel(requestId)`（hub.h:80）——取消在途请求。
- 告警模块 `src/alarm/` 是"第二个入口"的现成先例：外部事件源经 Ingress 驱动同一 agent 会话。
- 已链接 `OpenSSL::Crypto`（`CMakeLists.txt:52`），SHA-1/Base64 可直接用，WebSocket 握手**无新增依赖**。
- 自带 OpenAI 兼容 HTTP/HTTPS 客户端（ASR 转写复用其 HTTPS 通道，需扩展 multipart 上传；阶段 2 TTS 同理复用）、Skill 加载、会话持久化（`<workspace>/.crab/sessions/`，稳定 `sessionId` 跨进程重启恢复）。
- 现有 TCP 网关默认只监听 loopback；非 loopback 需显式配置且无认证（高风险开关）——本设计的 xiaozhi 网关**必须自带 token 认证**（见 §9）。

---

## 3. 硬约束

1. **LiteCrab 不能跑在 ESP32 上**：依赖 POSIX 进程（`exec_program`、Skill 脚本）、文件系统、线程、TLS；ESP32 是 MCU。因此 agent 只能跑在 Linux 主机（选定 5091），ESP32 保持"语音/显示终端"角色。
2. **5091 无 Python 3**：桥接逻辑必须用 C 写并交叉编译静态链接（沿用 LiteCrab 现有 5091 构建体系）。PC 侧 Python 仅用于开发期测试脚本。
3. **设备固件协议不可协商**：必须按 `docs/websocket.md` 实现 hello/listen/stt/tts 消息与 Opus 二进制帧的接收（阶段 1 缓存并转写）。

---

## 4. 总体架构

```
┌──────────────────┐   局域网 WebSocket (xiaozhi 协议)   ┌──────────────────────────────────────┐
│  xiaozhi-esp32    │ ─────────────────────────────────▶ │  3516 板                              │
│  设备（固件不改码） │   hello / listen / opus 音频(缓存)  │  litecrab_server（单静态二进制）        │
│                   │ ◀───────────────────────────────── │                                      │
│  屏幕显示对话文本   │   stt / llm / tts(纯文本)          │  src/gateway/xiaozhi_ws.c   ← 已实现  │
└──────────────────┘                                    │  src/gateway/ws_codec.c    （已实现） │
                                                        │  src/gateway/asr_sf.c      （已实现） │
         激活/检查点 HTTP POST                            │  src/gateway/tcp_line.c    （不动）   │
         CONFIG_OTA_URL ────────────────────────────────▶│  src/hub|session|kernel…   （不动）   │
                                                        │  （同端口提供 checkpoint JSON 应答）    │
                                                        │            │                          │
                                                        │            ▼ HTTPS                    │
                                                        │  云端 SiliconFlow（同一账号 key）：     │
                                                        │   ASR SenseVoiceSmall（免费，转写）    │
                                                        │   LLM Qwen3.5-35B-A3B（对话+工具）     │
                                                        └──────────────────────────────────────┘
```

要点：

- **一个端口双路由**：xiaozhi 网关监听的 TCP 端口同时服务两类请求——带 `Upgrade: websocket` 的连接走 WS 会话；普通 HTTP POST（激活检查点）返回把设备指向本端口的 JSON。设备从激活到对话全程只依赖 5091 一个地址。
- **设备固件零代码修改**：激活流程原样保留，只是 `CONFIG_OTA_URL` 指向 5091。
- **agent 内核零改动**（hub/session/kernel 不动）：新网关与 tcp_line 并列，通过 `IngressSubmit` 进入既有管道，复用会话、Skill、LLM、trace。
- **语音转文本前置**：上行 Opus 不丢弃——网关缓存，`listen stop` 时解码为 WAV 调 SiliconFlow ASR，转写文本再经 Ingress 进入 agent；ASR 失败回落固定桩文本（§7.4）。

---

## 5. 方案选型

| | 方案 A：LiteCrab 内置 xiaozhi WS 网关（**选定**） | 方案 B：独立桥进程 + loopback TCP | 方案 C：PC 上 Python 桥（仿 xiaozhi-esp32-server） |
|---|---|---|---|
| 形态 | `src/gateway/xiaozhi_ws.c`，与 tcp_line 并列 | 独立 C 程序，`127.0.0.1` 连 LiteCrab | Python 桥在 PC，agent 在 5091 |
| 部署 | **单二进制**，沿用现有 5091 交叉编译与 scp 流程 | 两个二进制，需双进程看护 | 三节点（设备/PC/5091） |
| 代码复用 | 复用 JSON、HTTP 客户端（ASR/TTS）、配置、Ingress | JSON/HTTP/配置需复制或抽公共库 | 最多，但引入 PC 节点 |
| 主要成本 | C 实现 WebSocket 服务端（握手 + 帧编解码，约 500 行，有界） | 进程管理 + 代码重复 | 与"全部上 5091"的决策冲突 |

选 A 的决定性理由：5091 的部署模式是"单静态二进制 + config + skills"，方案 B 破坏该模式且引入重复代码；方案 C 被部署决策排除。tcp_line（网络入口）与 alarm（事件入口）已证明 LiteCrab 的多入口架构成立，新增第三个入口是顺势而为。

---

## 6. 仓库组织

```
xiaozhi-esp32/
├── main/                       # 固件，阶段 1 不改代码
├── docs/litecrab/              # 本设计文档及后续部署说明
└── server/                     # 新增：服务端（5091 侧）
    ├── README.md               # 构建、部署、设备引导完整手册
    ├── litecrab/               # LiteCrab 源码副本（见下方同步策略）
    │   ├── src/gateway/xiaozhi_ws.c        # 新增：xiaozhi WebSocket 网关
    │   ├── src/gateway/ws_codec.c          # 新增：RFC6455 帧编解码 + 握手
    │   ├── src/gateway/asr_sf.c            # 新增：SiliconFlow ASR Provider（opus 解码→WAV→multipart 转写）
    │   ├── include/litecrab/xiaozhi_ws.h   # 新增：对 runtime 的注册接口
    │   └── …（LiteCrab 原有文件不动）
    └── tests/                  # 仅 PC 开发期使用（5091 无 Python）
        ├── fake_device.py      # 假设备：WS 客户端，驱动完整协议轮次
        ├── mock_llm.py         # mock OpenAI 兼容端点（可选，免真实 Key 跑集成测试）
        └── mock_asr.py         # mock 转写端点 /v1/audio/transcriptions（免真实 Key）
```

**LiteCrab 源码同步策略（2026-09-14 修订 2 起生效）**：`server/litecrab/` 是**唯一真源**——xiaozhi 网关的全部开发、测试、修复只发生在这里并随本仓库提交；上游 `LiteCrab_0829` 冻结为只读参考，不再直接修改。不使用 submodule/subtree。如需吸收上游后续演进，手动合并到 `server/litecrab`。

> 原（废弃）策略曾设计为"真源在 LiteCrab_0829、快照同步进本仓库"；现已反转，二者取其一，本仓库选定后者。

---

## 7. LiteCrab 侧详细设计

### 7.1 模块划分

| 文件 | 职责 | 依赖 |
|---|---|---|
| `src/gateway/ws_codec.c`（+ `include/litecrab/ws_codec.h`） | RFC 6455：握手应答计算（SHA-1+Base64）、帧解析（去掩码、分片重组、控制帧）、帧编码（发送侧） | OpenSSL Crypto（已链接） |
| `src/gateway/asr_sf.c`（+ `include/litecrab/asr_sf.h`） | SiliconFlow ASR Provider 实现：音频轮次缓冲、libopus 解码（Opus→PCM 16k/16bit/mono）、WAV 封装、multipart POST 转写、桩回落 | libopus（新增，静态链接）、现有 HTTP(S) 客户端 |
| `src/gateway/xiaozhi_ws.c`（+ `include/litecrab/xiaozhi_ws.h`） | 监听、连接状态机、xiaozhi JSON 收发、ASR Provider 调用、Ingress 对接、checkpoint HTTP 路由 | ws_codec、asr_sf、hub、json、config |
| `src/runtime/runtime.c`（小改） | 启动时按配置注册并启动 xiaozhi 网关线程（与 tcp_line 同级） | xiaozhi_ws |

`ws_codec` 与协议解耦、纯函数化，便于单测；`xiaozhi_ws` 不暴露细节，只向 runtime 提供 `XiaozhiWsStart(config)/XiaozhiWsRequestStop()`。

### 7.2 WebSocket 服务端（ws_codec）

实现 RFC 6455 服务端最小子集：

- **握手**：解析 `GET` 请求行与头；校验 `Upgrade: websocket`、`Connection` 含 `Upgrade`、`Sec-WebSocket-Version: 13`；取 `Sec-WebSocket-Key`，计算 `Sec-WebSocket-Accept = Base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))`，回 `101 Switching Protocols`。
- **帧解析**：FIN/opcode/长度（126→16 位扩展长度、127→64 位）；客户端帧**必须带掩码**（无掩码按协议错误断开）；4 字节 key 异或去掩码；支持 continuation 分片重组（单消息上限 `max_payload_bytes`，默认 256 KB，超限断开）；控制帧（close/ping/pong）可穿插在分片消息之间，payload ≤ 125 字节，ping 回 pong，close 回 close 后关 TCP。
- **帧编码**（发送侧）：文本帧承载 JSON，服务端发送不掩码；阶段 2 的音频用二进制帧。
- **认证**：握手阶段校验 `Authorization: Bearer <token>`（与 `require_auth`/`auth_token` 配置比对，见 §9）；失败回 `401` 并断开。

不依赖任何第三方 WS 库；SHA-1 用 `OpenSSL::Crypto`，Base64 用 `EVP_EncodeBlock`（或 50 行自实现，二选一，优先前者）。

### 7.3 xiaozhi 协议子集与连接状态机

每个连接一个状态机，在网关 worker 线程内阻塞驱动（`poll` + 读超时），模型与 tcp_line 的 worker 池一致（8 worker / 64 连接上限，可配置）：

```
ACCEPTED ──HTTP Upgrade+Auth──▶ WAIT_HELLO ──收到设备hello──▶ ┌──────┐
   │                                                            │ IDLE │
   │（非 Upgrade 的 HTTP POST → checkpoint 路由，见 §7.7）        └──┬───┘
   │                              listen start │  ▲  tts stop     │
   │                              （缓存opus）  ▼  │               │ listen stop
   │                                          LISTENING ◀────┐    │
   │                                              │          │    ▼
   │                            listen stop       │          │ abort
   │                                              ▼          │ PROCESSING
   │                                    ┌─ stt（ASR文本）─┐  │ （IngressSubmit
   │                                    │                │  │  等待 agent 回复）
   │                                    SPEAKING（纯文本） │  │
   │                                    tts start →       │  │
   │                                    sentence_start* → │──┘
   │                                    tts stop ────────▶ 回 IDLE/LISTENING
   └── 任意状态：TCP 关闭 / 协议错误 → 清理（在途请求 RequestCancel）
```

消息处理规则：

| 方向 | 消息 | 处理 |
|---|---|---|
| 收 | `hello` | 回服务端 hello：`{"type":"hello","transport":"websocket","session_id":"<sid>","audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}}`。`sid` 由 Device-Id 派生（见 §7.5）。忽略 `features`/`text_font`/`glyph_push` 等可选段 |
| 收 | `listen` `state=start` | → LISTENING；后续二进制帧缓存入 ASR Provider（字节计数 + `max_audio_seconds` 上限） |
| 收 | `listen` `state=detect` | 唤醒词事件，视同 start，忽略 `text` |
| 收 | `listen` `state=stop` | **ASR 触发点**（§7.4）：缓存音频解码→WAV→SiliconFlow 转写为文本（失败回落桩文本）→ 发 `stt` → IngressSubmit → PROCESSING |
| 收 | 二进制帧 | LISTENING 时喂给 ASR Provider 缓存；其他状态忽略（`tts start` 后设备本就停止上行） |
| 收 | `abort` | PROCESSING 中 → `RequestCancel(requestId)`；SPEAKING 中 → 补发 `tts stop`；回 IDLE |
| 收 | `mcp` | 阶段 1 网关不发起 MCP，收到即记日志忽略（不应出现） |
| 收 | 其他/畸形 JSON | 日志 + 忽略（对齐固件行为：缺 `type` 仅告警） |
| 发 | `stt` | `{"type":"stt","session_id":"…","text":"<ASR 转写文本>"}`，设备屏显用户话语 |
| 发 | `llm` | `{"type":"llm","session_id":"…","emotion":"happy","text":"😊"}`（stt 之后发一次，表情可选） |
| 发 | `tts` start/sentence_start/stop | 见 §7.6 |

**监听模式建议**：auto 模式下设备靠 VAD 判停，无人说话可能长时间不触发 `listen stop`；阶段 1 建议将设备设为 manual 模式（按键停止采集即触发转写）。两种模式网关行为一致，仅触发时机不同。

### 7.4 ASR Provider：agent 会话前的语音转文本

网关内定义唯一的输入侧扩展点（接口不变）：

```c
typedef struct XiaozhiAsrProvider {
    /* listen start 时调用：开始一轮采集（开缓冲） */
    void (*begin)(struct XiaozhiAsrProvider*, const char* session_id);
    /* 上行音频帧（缓存 opus 帧） */
    void (*feed_audio)(struct XiaozhiAsrProvider*, const uint8_t* opus, size_t len);
    /* listen stop 时调用：产出识别文本（转写结果或回落桩文本） */
    int (*finish)(struct XiaozhiAsrProvider*, const char* session_id,
                  char* text, size_t text_size);   /* 0=成功 */
    void (*abort)(struct XiaozhiAsrProvider*);
} XiaozhiAsrProvider;
```

**这是"agent 会话前先接 ASR、把需求转成文本"的落点**：设备真实采集上传，网关在 `listen stop` 时完成语音转写，文本经 `IngressSubmit` 进入 agent；设备与网关状态机、固件行为均不变。

- **`siliconflow_provider`（默认）**：`begin` 开缓冲；`feed_audio` 顺序缓存 Opus 帧（16 kHz 单声道，60 ms/帧）；`finish` 四步——
  1. **解码**：libopus 逐帧 `opus_decode` → PCM 16k/16bit/mono（新增静态依赖 libopus，仅用解码，armv7 可交叉编译）；
  2. **封装**：加 44 字节 WAV 头（固件上传的是**裸 Opus 帧、无容器**，WAV 是转写接口兼容性最好的格式）；
  3. **上传**：`multipart/form-data` POST `https://api.siliconflow.cn/v1/audio/transcriptions`，字段 `model=FunAudioLLM/SenseVoiceSmall` + `file=audio.wav`，请求头 `Authorization: Bearer $SILICONFLOW_API_KEY`，超时 `asr.timeout_ms`。**与 LLM 共用同一账号 key、不同端点**（LLM 是 `/v1/chat/completions`，ASR 是 `/v1/audio/transcriptions`）；
  4. **解析与回落**：响应 `{"text":"…"}` 取文本；HTTP 非 200 / 超时 / 空文本 → `fallback_to_stub=true` 时返回 `stub_input_text`，否则报错走 §7.6 道歉文案。

  SenseVoiceSmall **免费**（硅基流动价格页标注），中文识别效果好，整段转写约 1–2 s；免费档有并发/限流约束，单板 1–3 台设备足够。同接口换模型名即可切换 `Qwen/Qwen3-ASR-1.7B`、`XingChenAGI/XingChenASR V3.2`（均免费）。**密钥只经环境变量 `SILICONFLOW_API_KEY` 注入，不进仓库、不进配置文件**（对齐 `llm_config` 惯例与 §9）。
- **`stub_provider`（联调/回落）**：`begin/feed_audio/abort` 空操作（仅字节计数日志），`finish` 返回 `stub_input_text`。用于免 key 跑集成测试与 ASR 故障兜底。
- **阶段 3 可选 `mcp_asr_provider`**：`finish` 经 MCP 调 ASR 工具。

HTTP 客户端复用 LiteCrab 现有 OpenAI 兼容 HTTPS 通道，**新增 multipart/form-data 构造**（现实现为 JSON body，扩展约百行 C）；JSON 解析复用现有工具。

### 7.5 Agent 对接（Ingress）

- **身份映射**：
  - `userId` = `xiaozhi:<Device-Id MAC>`（握手头取得，写入会话供 trace 审计）；
  - `sessionId` = `xiaozhi-<Device-Id 去冒号小写>`，**跨连接稳定**——设备每次唤醒新建 WS 连接，靠该 ID 让 LiteCrab 恢复同一会话（`.crab/sessions/` 快照，跨进程重启亦恢复），与设备侧 `session_id` 回显字段同值。
- **提交**：`listen stop` 后——

```c
IngressOptions opt = { .source      = "gateway:xiaozhi-ws",
                       .userId      = /* §映射 */,
                       .sessionId   = /* §映射 */,
                       .type        = LITE_MSG_CHAT,
                       .priority    = LITE_PRIORITY_NORMAL,
                       .replyMode   = LITE_REPLY_SYNC };
IngressSubmit(asr_text, len, &opt, &result);      /* hub.h:74，asr_text 为 §7.4 转写结果 */
/* worker 线程内阻塞等待回复 */
MessageBusTimedPopOutboundByRequestId(result.requestId, &reply,
                                      agent_reply_timeout_ms);  /* hub.h:72 */
```

  超时/取消路径调 `RequestCancel(result.requestId)`（hub.h:80），**精确按 requestId 取消**，不影响并发的告警投递。
- **串行性说明**：LiteCrab 出站 worker 为 1，语音请求与告警投递在总线排队（告警 `LITE_PRIORITY_HIGH` 优先）。PLC_Diagnosis 长耗时运行期间，新语音请求将等待——阶段 1 可接受，文档明示；后续可按需调整优先级或并发度。
- **首条消息预开 会话**（可选）：连接建立后主动提交 `LITE_MSG_SESSION_OPEN`（ACK_ONLY），对齐 tcp_line 的会话预热行为，非必需。

### 7.6 回复下发

agent 回复文本按句拆分后走 xiaozhi 文本通道（无音频帧）：

1. `{"type":"tts","state":"start"}` → 设备进入说话态、停止上行；
2. 逐句 `{"type":"tts","state":"sentence_start","text":"<句>"}` → 屏幕聊天气泡；
3. `{"type":"tts","state":"stop"}` → 设备回 idle（auto 模式回 listening）。

分句规则：按 `。！？!?；;\n` 切分，单句上限 120 字符（超长硬切，UTF-8 不拆半字符），空句跳过。JSON 字符串由 LiteCrab 现有 json 工具转义，UTF-8 直传（固件 cJSON 原生支持）。

错误路径同样有文字反馈：agent 超时/失败时发 `tts start` + `sentence_start("抱歉，处理超时，请稍后再试")` + `tts stop`，避免设备停在 listening 无响应。

### 7.7 设备引导：激活检查点端点

同端口识别普通 HTTP 请求（首请求无 `Upgrade` 头）：

- `POST /`（任意路径，即 `CONFIG_OTA_URL` 指向的地址）：返回 200 + JSON：

```json
{
  "server_time": 1760000000,
  "websocket": {
    "url": "ws://<advertised_ip>:<port>/xiaozhi/v1/",
    "token": "<auth_token>",
    "version": 1
  }
}
```

- 响应**不含** `mqtt` 段 → 固件 `InitializeProtocol()` 选择 `WebsocketProtocol`（application.cc:527-534）；不含 `firmware`/`activation` 段 → 无升级、无激活挑战，激活直接完成进 idle。
- `advertised_ip` 为配置项（网关自身探测的出口地址仅作默认值），确保写入设备的 URL 从设备可达（跨网段/NAT 场景手工指定）。
- 固件侧唯一动作：构建时把 `CONFIG_OTA_URL` 设为 `http://<5091-ip>:<port>/`（sdkconfig 配置项，非代码修改），刷一次固件；此后每次开机设备自动从 5091 取到 WebSocket 地址。
- 备选（不推荐）：用 esptool/NVS 工具直写 `websocket/url`。**已知坑**：`InitializeProtocol()` 依据的是**本次 checkpoint 响应**里的段决定传输类型（而非 NVS 既有值），若 checkpoint 失败会落入 MQTT 缺省分支导致连不上，故设备仍需可达的 checkpoint 端点——统一走本节主方案最稳。

### 7.8 MCP 预留（阶段 3）

- 固件 hello 中 `features.mcp: true`；网关阶段 1 不发 `initialize`，设备 MCP 服务闲置，无副作用。
- 阶段 3 设计要点（本文档仅占位）：网关收到固件 MCP `initialize` 应答后，将 `tools/list` 结果注册为 LiteCrab 的动态工具（命名空间 `xiaozhi.device.*`），`tools/call` 的 JSON-RPC 经 WS 透传；反之 ASR/TTS 亦可用 MCP client 形态接入。实现时需补齐 JSON-RPC id 关联表与超时。

---

## 8. 配置设计

`config/base_config.json` 新增段（遵循"默认值 < JSON < 环境变量 `LITECRAB_*` < 命令行"既有优先级）：

```json
{
  "xiaozhi_ws": {
    "enabled": true,
    "listen_ip": "0.0.0.0",
    "listen_port": 8000,
    "advertised_ip": "",
    "require_auth": true,
    "auth_token": "",
    "agent_reply_timeout_ms": 120000,
    "max_payload_bytes": 262144,
    "worker_threads": 8,
    "max_connections": 64,
    "idle_timeout_ms": 300000,
    "asr": {
      "provider": "siliconflow",
      "endpoint": "https://api.siliconflow.cn/v1/audio/transcriptions",
      "model": "FunAudioLLM/SenseVoiceSmall",
      "api_key_env": "SILICONFLOW_API_KEY",
      "timeout_ms": 15000,
      "max_audio_seconds": 60,
      "fallback_to_stub": true,
      "stub_input_text": "使用 PLC_Diagnosis 检查当前活动告警并给出处理建议"
    }
  }
}
```

| 键 | 说明 |
|---|---|
| `enabled` | 默认 false；显式开启才监听（对齐告警"连接参数只配置不启动"的惯例） |
| `listen_ip` | 非 loopback 是本网关的设计前提（设备要连），但仍显式配置留痕 |
| `advertised_ip` | checkpoint 响应里写入设备的 ws 地址用；空=自动探测 |
| `require_auth`/`auth_token` | true 时握手必须带匹配 Bearer token（§9） |
| `asr.provider` | `siliconflow`（默认，真实转写）或 `stub`（固定文本，免 key 联调） |
| `asr.endpoint`/`asr.model` | OpenAI 兼容转写端点与模型名；切 Qwen3-ASR/XingChenASR 只改这里 |
| `asr.api_key_env` | 取 key 的环境变量名，默认 `SILICONFLOW_API_KEY`（与 LLM 共用同一账号）；**密钥不落仓库** |
| `asr.timeout_ms`/`asr.max_audio_seconds` | 转写请求超时；单轮音频缓存上限（Opus 约 200 KB/分钟） |
| `asr.fallback_to_stub`/`asr.stub_input_text` | ASR 失败时回落固定文本，及其内容 |

命令行覆盖示例：`--xiaozhi-ws-port 8000`、`--xiaozhi-ws-asr-provider stub`；环境变量：`LITECRAB_XIAOZHI_WS_AUTH_TOKEN`、`SILICONFLOW_API_KEY`（ASR/LLM 共用）。

示例配置放入 `config/base_config.example.json` 注释说明（密钥不落仓库，对齐 `llm_config` 惯例）。

---

## 9. 安全设计

威胁模型：局域网内任意主机可访问 5091 该端口；LiteCrab 拥有文件读写与 `exec_program` 能力，**语音入口即 agent 入口**，必须认证。

1. **认证**：设备握手自带 `Authorization: Bearer <token>`（固件行为，websocket_protocol.cc:97-103，token 来自 checkpoint 下发）。`require_auth: true` 时校验，不匹配回 401 断开。token 经 checkpoint 下发，不手工刷进设备。
2. **与 tcp_line 的关系**：现有 TCP 网关维持 loopback 默认不变；xiaozhi 网关是**独立监听**的第二个服务，不复用 `allow_unauthenticated_remote` 开关。
3. **传输**：局域网明文 ws://。风险（同网段窃听/伪造）由 token 缓解；5091 与设备同网段部署时接受。后续如需 wss，可在 ws_codec 握手层加 TLS（OpenSSL 已链接），阶段 1 不做。
4. **输入边界**：`max_payload_bytes` 硬上限防内存攻击；畸形帧/越界长度立即断开；ASR 缓冲受 `asr.max_audio_seconds` 上限约束，转写文本与 `stub_input_text` 受 `INGRESS_MAX_REQ_BYTES`（16 KB）约束。
5. **日志**：记录 Device-Id、连接起止、ASR 触发与耗时/字节数、agent requestId 与耗时；不记录 token 明文、音频内容与转写全文。
6. **隐私边界**：用户语音经 HTTPS 出局域网至 SiliconFlow（SenseVoiceSmall）转写；音频转写后即弃、不落盘、不留存。部署手册需向使用者明示此数据流向；如需全离线，可切换 provider 至本地 FunASR runtime（另行设计）。

---

## 10. 并发与资源

- worker 池模型复制 tcp_line：8 worker × 64 排队连接上限；worker 持有一条连接直到其关闭（语音连接为分钟级占用），即**并发对话数=worker 数（8）**，超出进入待服务队列。单板场景（1–3 台设备）足够。
- 每连接内存：读缓冲 4 KB（hello/JSON）+ 分片重组缓冲（上限即 `max_payload_bytes`）+ ASR 音频缓冲（Opus 帧缓存，`max_audio_seconds=60` 约 200 KB；`finish` 时临时 PCM 解码缓冲约 1.9 MB，转写结束即释放）。
- **单 agent 串行**：同一时刻仅处理一个 chat 请求（出站 worker=1），语音与告警经总线优先级排队。设计上明确接受，避免并发改动内核。

---

## 11. 错误处理与超时

| 场景 | 行为 |
|---|---|
| 设备 hello 超时（15 s 未收到） | 断开连接，日志 |
| 握手认证失败 | HTTP 401，断开 |
| 帧协议错误（无掩码/超长/坏 opcode） | 尽力回 close 帧，断开 |
| ASR HTTP 非 200 / 超时（`asr.timeout_ms`）/ 响应无 text | `fallback_to_stub=true`：回落桩文本，发 `stt` + 提示前缀（如"（语音识别失败，使用预设指令）"），流程继续；`false`：发道歉文案，回 IDLE。日志记 HTTP 状态码 |
| ASR 返回空文本（用户未说话/纯噪音） | 视同本轮无输入：不发 `stt`，直接回 IDLE（省一次 LLM 调用） |
| 单轮音频超 `asr.max_audio_seconds` | 截断缓冲，日志告警，仍走转写 |
| `SILICONFLOW_API_KEY` 未设置且 provider=siliconflow | 启动即失败退出（fail fast） |
| agent 回复超时（`agent_reply_timeout_ms`，默认 120 s） | `RequestCancel` + 文字道歉（§7.6），回 IDLE |
| agent 返回错误 | 同上，文本含错误摘要 |
| 设备在 PROCESSING 中 abort/断开 | `RequestCancel(requestId)`，静默清理 |
| LLM 配置缺失/非法 | 网关启动即失败退出（fail fast），不进入半可用状态 |
| checkpoint POST 非 200 期望路径 | 回 404/400，日志（不影响 WS 路由） |

---

## 12. 测试计划

**单元测试**（并入 LiteCrab 现有 CTest，WSL x86 运行）：

- ws_codec：握手 Accept 计算（RFC 6455 官方向量 `dGhlIHNhbXBsZSBub25jZQ==` → `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`）；带掩码帧往返、分片重组、控制帧穿插、长度边界（125/126/65535/65536）、畸形帧拒绝。
- xiaozhi 消息构建/解析：hello/stt/tts JSON 序列化快照；分句器（中英文标点、超长硬切、UTF-8 不拆半字符）。
- asr_sf：WAV 头封装字节快照；multipart 请求体构造快照（boundary/Content-Disposition）；`{"text":…}` 解析；HTTP 500/超时/空文本三条错误路径各自回落行为；`stub_provider` 返回配置文本。
- checkpoint 响应体生成。

**集成测试**（PC 开发期，`server/tests/`，不部署 5091）：

- `mock_llm.py`：本地起 OpenAI 兼容 HTTP 端点返回固定 completion，`llm_config` 指向它——集成测试不烧真实 Key。
- `mock_asr.py`：本地起 OpenAI 兼容转写端点（`/v1/audio/transcriptions`）返回固定文本，`asr.endpoint` 指向它——ASR 链路也不烧真实 Key。
- `fake_device.py`：完整模拟设备行为——HTTP POST 激活 → 断言 checkpoint JSON → WS 连接（带 Bearer 头）→ hello → 断言服务端 hello → `listen start` → 发 N 个二进制帧（联调期可发预制的 TTS/编码器产物 Opus 帧，mock_asr 断言收到的 WAV 可解码）→ `listen stop` → 断言依次收到 `stt`(文本==mock ASR 固定文本) / `tts start` / ≥1 个 `sentence_start`(文本==mock LLM 回复) / `tts stop`。另测：401、abort 取消、ASR 失败回落桩、超时路径、并发两设备连接。
- 依赖：Python `websockets`（pip，仅开发机）。

**真机验证**（阶段 1 收尾）：

1. `sdkconfig` 设 `CONFIG_OTA_URL` 指向 5091，刷机；
2. 设备联网后自动激活，日志确认选择 WebsocketProtocol；
3. 唤醒（建议 manual 模式）→ 说话（如"使用 PLC_Diagnosis 检查当前活动告警"）/按键停止 → 屏幕依次显示：用户话语（**ASR 实时转写文本**，核对与所说一致）→ agent 回复分句；
4. 5091 侧 `tail -F logs/litecrab.log` 核对 ASR 耗时/字节数、Ingress、requestId 与 trace（`JSONL`）落盘。

---

## 13. 构建与部署（目标板）

> 2026-09-14 修订 2：目标板由 5091 改为 **3516**；本节命令沿用原 5091 静态交叉编译体系，
> 3516 实际部署步骤（含 libopus 交叉编译、CA 配置）见 `server/README.md`。
> WSL x86 开发验证环境（含全部测试）亦见 `server/README.md`。

完全沿用 LiteCrab 现有 5091 体系；新增依赖仅 **libopus**（静态链入 `litecrab_server`，只用解码 `opus_decode`，源码或 armv7 预编译静态库均可，进 CMake 随仓库）：

```sh
# WSL 交叉编译（server/litecrab 目录内）
cmake -S . -B build-5091 \
  -DCMAKE_TOOLCHAIN_FILE=aarch32-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DLITECRAB_BUILD_TESTS=OFF \
  -DLITECRAB_ENABLE_STATIC_LINK=ON
cmake --build build-5091 -j
arm-linux-gnueabihf-readelf -d build-5091/litecrab_server | grep NEEDED || echo "全静态"

# 部署（增量：仅二进制与配置）
scp build-5091/litecrab_server enspire@<5091-ip>:/mnt/home/enspire/litecrab/
scp config/*.json enspire@<5091-ip>:/mnt/home/enspire/litecrab/config/

# 板上运行（SILICONFLOW_API_KEY 为 LLM/ASR 共用的同一账号 key，勿入仓库）
export SSL_CERT_FILE=/mnt/home/enspire/litecrab/ca-bundle.crt
export SILICONFLOW_API_KEY=your-key
export LITECRAB_XIAOZHI_WS_AUTH_TOKEN=<随机token>
./litecrab_server --llm-config config/llm_config.json \
  --llm-provider siliconflow --workspace .
```

`server/README.md` 落地完整手册（含防火墙放行端口、token 生成、固件 sdkconfig 步骤）。

---

## 14. 阶段规划与工作量

| 阶段 | 内容 | 预估 |
|---|---|---|
| 1a | ws_codec + 单测 | 2–3 人日 |
| 1b | xiaozhi_ws 网关（状态机、Ingress、分句、checkpoint 路由）+ 单测 | 3–4 人日 |
| 1b' | ASR：libopus 集成与解码、WAV/multipart 构造、siliconflow/stub provider 与回落 + 单测 | 2–3 人日 |
| 1c | 配置/runtime 注册/文档 | 1 人日 |
| 1d | mock_llm + mock_asr + fake_device 集成测试 | 1–2 人日 |
| 1e | 真机联调（sdkconfig、激活、真实语音转写、对话轮次） | 2–3 人日 |
| **阶段 1 合计** | | **约 2.5 周** |
| 2 | TTS 下行（HTTP 合成 + 二进制 Opus 帧、协商 v2/v3 头）；可选流式 ASR 降延迟 | 另行设计 |
| 3 | MCP 双向桥（设备工具注册进 LiteCrab、ASR/TTS 可选 MCP 化） | 另行设计 |

## 15. 风险与开放问题

| # | 风险/问题 | 影响 | 缓解 |
|---|---|---|---|
| R1 | checkpoint 最小响应的兼容性未真机验证（固件解析路径多，可能有隐含必填段） | 设备激活失败 | 集成期优先真机冒烟；兜底方案为固件小改（跳过 OTA 直连），改动限制在用户自己的 fork |
| R2 | auto 模式 VAD 判停不稳、长静音产生近空音频 | 演示体验 | 以 manual 模式为主；ASR 空文本直接回 IDLE（§11），不误触发 agent |
| R3 | PLC_Diagnosis 长耗时（分钟级），设备停在 listening 等待 | 用户体验 | 已发 stt 提示在处理；阶段 2 可加进度 sentence_start 播报 |
| R4 | 单 agent 串行，语音与告警排队 | 高峰延迟 | 总线优先级已有；必要时提升告警、语音分道 |
| R5 | ws 实现边角（分片、控制帧、大帧）引发断连 | 稳定性 | 单测覆盖 RFC 向量；fake_device 长稳压测 |
| R6 | 5091 需公网出口访问 `api.siliconflow.cn`；免费档限流或模型调整 | ASR 不可用 | `fallback_to_stub` 保底演示；`endpoint`/`model`/`api_key_env` 可配置切换（Qwen3-ASR/XingChenASR/本地 FunASR runtime）；部署前 curl 冒烟验证 |
| R7 | ASR 整段转写延迟（上传 + 识别约 1–2 s）叠加 LLM 延迟 | 交互偏慢 | manual 模式用户有按键预期，可接受；阶段 2 可选流式 ASR 优化 |
| O1 | ~~ASR 选型~~ **已定**：SiliconFlow SenseVoiceSmall（免费、OpenAI 兼容、整段转写） | — | 待真机验证免费档并发与音频长度上限；provider 抽象可无痛切换 |
| O2 | 阶段 2 TTS：5091 上做 Opus 编码的开销 | — | 阶段 2 评估，必要时 TTS 服务直接回 Opus |
| O3 | 跨网段部署时 advertised_ip 与设备可达性 | — | 配置项已留；阶段 1 限定同网段 |

---

## 附录 A：一轮完整交互时序（阶段 1）

```
设备                                   网关(litecrab_server)
 │── HTTP POST / (激活检查点) ─────────▶│ 200 {"websocket":{"url":"ws://…:8000/xiaozhi/v1/","token":"…","version":1}}
 │                                     │（无mqtt段→固件选WebsocketProtocol）
 │── WS握手(Authorization:Bearer …) ──▶│ 校验token → 101 Switching Protocols
 │── {"type":"hello", features…} ─────▶│
 │◀─ {"type":"hello","transport":"websocket","session_id":"xiaozhi-aabbcc…","audio_params":{opus/16k/1/60ms}}
 │── {"type":"listen","state":"start","mode":"manual"} ─▶│ 进入LISTENING
  │── (binary opus × N) ────────────────▶│ 缓存入 ASR Provider          [ASR-Provider.begin/feed]
  │── {"type":"listen","state":"stop"} ─▶│ ★opus 解码(libopus)→PCM→WAV
  │                                     │   →HTTPS POST SiliconFlow
  │                                     │   SenseVoiceSmall 转写（约1–2s，失败回落桩文本）
  │◀─ {"type":"stt","text":"<转写文本>"} ─│
  │                                     │ IngressSubmit(转写文本, sessionId=xiaozhi-<mac>) →
  │                                     │   LLM/工具(PLC_Diagnosis…) → 同步取回复
 │◀─ {"type":"llm","emotion":"happy"} ──│
 │◀─ {"type":"tts","state":"start"} ────│
 │◀─ {"type":"tts","state":"sentence_start","text":"<回复句1>"} 
 │◀─ {"type":"tts","state":"sentence_start","text":"<回复句2>"} 
 │◀─ {"type":"tts","state":"stop"} ─────│ 设备回idle，本轮结束
```

## 附录 B：术语与文件索引

- xiaozhi 协议：`docs/websocket.md`、`docs/mcp-protocol.md`（本仓库）
- SiliconFlow ASR：`api-docs.siliconflow.cn`（`POST /v1/audio/transcriptions`，OpenAI 兼容 multipart；免费模型 `FunAudioLLM/SenseVoiceSmall`、`Qwen/Qwen3-ASR-1.7B`、`XingChenAGI/XingChenASR V3.2`；与 `/v1/chat/completions` 共用账号 key）
- libopus：`opus_decode`（解码 16k/mono/60ms 帧 → PCM）
- 固件关键点：`main/ota.cc:46-52,85-107,168-186`；`main/application.cc:354-369,527-534`；`main/protocols/websocket_protocol.cc:79-106`
- LiteCrab 关键点：`include/litecrab/hub.h:72-80`（Ingress/等待/取消）；`src/gateway/tcp_line.c`（worker 池先例）；`CMakeLists.txt:52`（OpenSSL）；README「5091 交叉编译与运行」章节
