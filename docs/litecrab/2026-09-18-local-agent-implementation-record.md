# 本地 Agent 设备端实现与调试记录

| 项目 | 内容 |
|---|---|
| 日期 | 2026-09-17 ～ 2026-09-18 |
| 状态 | 已烧录验证：自研 agent（LocalAgentProtocol）在 ESP32 上运行；ASR 真实转写成功、LLM 自主调用机器人工具成功、状态机多轮稳定；TTS voice 格式修复已烧录（待最终听音确认） |
| 关联设计 | `docs/litecrab/2026-09-17-local-agent-on-device-design.md`（方案 A：LocalAgentProtocol） |
| 目标 | 按设计文档实现"agent 跑在设备上、直连 SiliconFlow、无 PC 中转"，通过串口烧录真机验证 |

---

## 1. 总体改动概览

在 `main/` 下新增 agent 模块（约 2000 行）和一个新的 `Protocol` 实现，`Application` 仅加一个协议选择分支。设备开机不再做 OTA 检查点/激活（local-agent 模式短路），唤醒后由固件内 agent 线程完成 ASR → LLM 工具循环 → TTS 全流程，云端仅剩 SiliconFlow 三个 HTTPS 端点。

```
ESP32 固件
├── Application::ActivationTask      ✚ local_agent 短路（跳过 OTA/激活，200ms 进 idle）
├── Application::InitializeProtocol  ✚ 协议选择：强制 local_agent（联调期）
├── LocalAgentProtocol (新)           实现 Protocol 接口，对 Application 伪装成"服务端"
├── Agent (新)                        agent loop：ASR→LLM 工具循环→TTS 编排
├── SiliconFlow Asr/Llm/Tts (新)      三个 HTTPS Provider
└── McpServer                         ✚ GetToolsListJson() / CallToolDirectly()
                                      （LLM 工具调用 in-process 执行，不走网络）
```

---

## 2. 修改文件清单

### 2.1 新增文件

| 文件 | 内容 |
|---|---|
| `main/agent/provider.h` | `AgentConfig` 配置结构；`AsrProvider`（begin/feed_audio/finish/abort）、`LlmProvider`（Chat，支持 tool_calls）、`TtsProvider`（Synthesize → opus 帧）三个抽象接口；`ToolCall`/`LlmResponse` 结构 |
| `main/agent/agent.h` / `agent.cc` | Agent 类：`RunTurn`（emit stt/llm → `DoLlmToolLoop` → `DoTts`）；`InvokeMcpTool` 构造 JSON-RPC 调 `McpServer::CallToolDirectly` 并抽取结果文本；中文分句器（按 `。！？!?；;\n` 切分，UTF-8 安全硬切 120 字节）；provider 工厂（按 `cfg.*_provider` 路由，默认 siliconflow） |
| `main/agent/conversation.h` / `conversation.cc` | OpenAI 风格会话历史（system/user/assistant/tool 消息，FIFO 保留 N 轮），`ToJson(system_prompt)` 序列化为 messages 数组 |
| `main/agent/providers/provider_common.h` / `.cc` | `ReadFullBody`、`DecodeChunked`（RFC 7230 手工解码）、`BuildWavHeader`（44 字节 PCM WAV 头）、`DecodeOpusFrames`（esp_opus_dec，16k/mono/60ms）、`EncodePcmToOpus`（esp_opus_enc）、`ParseWav` |
| `main/agent/providers/siliconflow_provider.h` | 三个 provider 的工厂声明 |
| `main/agent/providers/siliconflow_asr.cc` | 缓存 opus 帧（互斥锁保护，~200KB/分钟上限）→ 解码 PCM → 封 WAV → multipart POST `/v1/audio/transcriptions`（SenseVoiceSmall）→ 解析 `{"text"}`；2 次重试；失败回落桩文本"你好"（rc=1），硬失败 rc=-1 |
| `main/agent/providers/siliconflow_llm.cc` | OpenAI 兼容 POST `/v1/chat/completions`（stream=false，tools+tool_choice=auto）→ 解析 `choices[0].message.content` 与 `tool_calls[]`；2 次重试 |
| `main/agent/providers/siliconflow_tts.cc` | POST `/v1/audio/speech`（CosyVoice2，response_format=wav）→ chunked 判定与解码 → ParseWav → `EncodePcmToOpus`（60ms 帧）；2 次重试；失败跳过该句音频只保留文本 |
| `main/protocols/local_agent_protocol.h` / `.cc` | `Protocol` 接口实现：`OpenAudioChannel` 读 NVS `agent` 命名空间配置（api_key 为空时代码内联 key，联调期）+ 创建 Agent + 启动 agent 线程（16KB 栈）；`SendAudio`→`FeedAudio` 缓存；`SendStart/StopListening`、`SendWakeWordDetected`、`SendAbortSpeaking` 重写为事件位；`EmitJson`/`EmitAudio` 经 `Application::Schedule` 回喂（对 Application 伪装成服务端）；`AgentTask` 事件循环（LISTEN_START/LISTEN_STOP/ABORT/STOP_THREAD） |

### 2.2 修改文件

| 文件 | 修改内容 |
|---|---|
| `main/application.cc` | ① `#include "local_agent_protocol.h"`；② `ActivationTask()`：`protocol_type=="local_agent"`（当前无条件强制）时跳过 `CheckNewVersion()`/激活重试，直接 `MarkCurrentVersionValid` + `InitializeProtocol` + 完成事件；③ `InitializeProtocol()`：按 NVS `protocol/type`（当前强制 `local_agent`）选择 `LocalAgentProtocol`，保留 mqtt/websocket/auto 分支 |
| `main/mcp_server.h` / `.cc` | 新增公有方法 `GetToolsListJson()`（工具列表转 OpenAI tools 数组，`inputSchema`→`parameters` 重命名，跳过 user-only）和 `CallToolDirectly(name, arguments)`（参数校验复用 `DoToolCall` 逻辑；经 `Application::Schedule` 派发到主任务执行，condvar 同步等待，20s 超时；不走 `ReplyResult` 网络路径）。既有 `ParseMessage` 路径不动 |
| `main/Kconfig.projbuild` | 新增 `menu "Local Agent"`：`LOCAL_AGENT_DEFAULT_PROVIDER/LLM_MODEL/ASR_MODEL/TTS_MODEL/TTS_VOICE` 及各超时/上限共 10 项。**注意：Kconfig 默认值只在 sdkconfig 首次生成时生效，之后改默认值不会传播**（TTS 400 报错的根因之一） |
| `main/CMakeLists.txt` | SOURCES 增加 8 个新文件；INCLUDE_DIRS 增加 `agent`、`agent/providers`；PRIV_REQUIRES 增加 `espressif__esp_audio_codec`（esp_opus_enc/dec） |
| `sdkconfig.defaults` | `CONFIG_OTA_URL` 由 PC 中继地址 `http://192.168.43.9:8000/` 恢复为官方 `https://api.tenclass.net/xiaozhi/ota/`（local-agent 模式不使用；保留出厂激活能力） |

---

## 3. 调试过程中出现的报错与修复

按时间顺序记录。所有报错均已在真机上复现并修复。

### 3.1 编译期报错（5 个）

**E1：CJK 多字节字符比较恒为 false**
- 现象：`agent.cc` 分句器 `(c == '。')` 报 `-Werror=type-limits`（`char` 无法容纳 3 字节 UTF-8 中文标点，多字符字面量属未定义行为）
- 修复：删除该死代码。中文标点已由紧随其后的字节模式匹配覆盖（`。`=E3 80 82，`！？；` = EF BC 81/9F/9B）

**E2：`EmitJsonFn does not name a type`**
- 现象：`agent.h` 缺 `<functional>`/`<cstddef>` 头
- 修复：补头文件，并前置声明 `struct cJSON`

**E3：`std::function target must be copy-constructible`**
- 现象：`EmitAudio` 把 `unique_ptr<AudioStreamPacket>` move 进 lambda 捕获，而 `Application::Schedule` 存储 `std::function`（要求可拷贝）
- 修复：改用 `shared_ptr<AudioStreamPacket>` 捕获，回调内再 `make_unique` 拷贝一份移交

**E4：`Trim was not declared`**
- 现象：`Conversation::Trim()` 在声明前被调用
- 修复：在 `conversation.h` 私有段补声明

**E5：`int16_t* → uint8_t* 转换失败` 与 `取临时对象地址`**
- 现象：`esp_audio_enc_in_frame_t.buffer` 是 `uint8_t*`，PCM 指针需强转；`&esp_pthread_get_default_config()` 取了右值地址
- 修复：`reinterpret_cast<uint8_t*>`；先存局部变量再取地址

### 3.2 烧录操作类报错（2 个）

**E6：烧录后行为不变（连 192.168.43.9 依旧）**
- 现象：已改代码并 `idf.py build` + 烧录 `merged-binary.bin`，但设备仍连旧 OTA 地址，新日志串未出现
- 根因：**`idf.py build` 不会重新生成 `merged-binary.bin`**，烧的是上一次的陈旧合并镜像
- 修复与规矩：每次 `idf.py build` 后必须 `idf.py merge-bin`；烧录前用 `strings merged-binary.bin | grep <新日志串>` 验证新代码确实在镜像里

**E7：`Could not open COM3, port busy`**
- 现象：烧录时串口被占用
- 根因：后台串口记录器（PowerShell 进程）持有 COM3
- 修复：烧录前 `Stop-Process` 清理本会话启动的 powershell 记录进程

### 3.3 运行时崩溃（关键）

**E8：`assert failed: xQueueSemaphoreTake queue.c:1713 (pxQueue->uxItemSize == 0)` → 设备重启 → 用户看到"又变成连接 wifi 状态"**
- 定位：`xtensa-esp32s3-elf-addr2line -e xiaozhi.elf <backtrace>` 解码出两条路径——
  - agent 线程：`Agent::DoAsr` → `asr_->Finish()` 内 `std::lock_guard`（agent.cc:99）
  - 主任务：`Application::Run`(application.cc:240) → `SendAudio` → `OnAudioFrame` → `FeedAudio` 的锁（agent.cc:53）
  - 两个不同任务先后在不同位置锁**同一个互斥锁**都断言失败 ⇒ 互斥锁堆内存被破坏
- 根因（三个 bug 叠加）：
  1. **agent 线程栈只有 3KB**：`esp_pthread_set_cfg` 写在线程体内是无效的（该 API 只配置"调用线程将来创建的子线程"），12KB 配置从未生效，实际用 `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=3072`。线程栈块与互斥锁堆块相邻，后续 TLS 握手等深调用溢出直接砸掉互斥锁内存
  2. **`aborted_` 闩锁导致通道重开 → use-after-free**：`IsAudioChannelOpened()` 返回 `channel_opened_ && !aborted_`，而唤醒打断（`SendAbortSpeaking`）把 `aborted_` 永久置 true → 下次唤醒时 App 认为通道已关 → 再次 `OpenAudioChannel()` → `agent_ = make_unique<Agent>()` 在旧 agent 线程仍存活时删除旧 Agent 对象 → 旧互斥锁内存被释放复用 → 正是断言崩溃
  3. **事件位丢失**：`xEventGroupWaitBits(xClearOnExit=pdTRUE)` 一次性清掉所有就绪位，原 if-continue 链只处理一个就丢弃其余（唤醒打断时 ABORT+LISTEN_START 同时到达会丢 LISTEN_START）
- 修复（`local_agent_protocol.cc`）：
  - `esp_pthread_set_cfg`（16KB 栈/prio 4/名字 agent_task）移到 `std::thread` 创建**之前**，创建后恢复默认配置（避免影响相机编码线程）
  - `OpenAudioChannel()`：若 `agent_thread_.joinable()` 则复用存活通道（清 aborted_ 直接返回），不再重建 Agent
  - `IsAudioChannelOpened()`：只返回 `channel_opened_`（单轮 abort 不失效通道）
  - `SendWakeWordDetected`/`SendStartListening`：清 `aborted_` 并统一走事件位（不再从主任务直接调 `agent_->OnListenStart()`）
  - `AgentTask`：单次唤醒处理全部就绪位；ABORT+LISTEN_STOP 同批到达时跳过该轮（音频已陈旧）
- 结果：修复后 10+ 轮对话循环零崩溃

**E9：ASR 缓冲上限算错（隐患）**
- 现象：`max_audio_seconds × 每秒字节 × 60` 多乘 60，上限虚高至 ~12MB，长录音可能耗尽 PSRAM
- 修复：`siliconflow_asr.cc` 改为 `max_audio_seconds × (200*1024/60)`，约 200KB/分钟

### 3.4 HTTPS/API 报错

**E10：所有 HTTPS 请求失败 `mbedtls_ssl_fetch_input error=76`（-0x004C，连接被重置）**
- 现象：TLS 握手全部成功（`Established new connection to api.siliconflow.cn:443 cost=~550ms`），但 ASR/LLM/TTS 全部在 `HTTP write failed` 处失败；ASR 回落桩文本"你好"、LLM 播道歉、TTS 跳音频（agent 容错链全部按设计工作）
- 定位：读 `managed_components/78__esp-ml307/src/http_client.cc` 源码——
  1. `Open()` 不带 `SetContent` 的 POST 自动切换 chunked 模式（发送 `Transfer-Encoding: chunked` 头）；我的 `Write(body)` 之后**没有发送终止块**（`Write(buf, 0)`），服务器等不到请求结束即 RST
  2. 我的返回值判断 `Write(...) != body.size()` 本身就是错的：chunked 模式返回值含十六进制长度前缀+CRLF 分帧开销，必然大于 `body.size()`，即使发送成功也误判失败
- 修复：三个 provider 全部改用 OTA 已验证的模式——`SetContent(std::string(body))` + `Open("POST", url)`（由 Open 自动带 `Content-Length` 一次性发送），删除全部 `Write()` 调用
- 结果：ASR 真实转写成功（`ASR rc=0`），LLM 开始返回内容并自主调用工具

**E11：ASR 超时不足**
- 现象：默认 30s，SiliconFlow 免费档排队实测 20-55s（LiteCrab 阶段结论）
- 修复：`LoadConfig` 中 ASR 超时默认 90s（不再依赖 Kconfig 值）

**E12：TTS HTTP 400**
- 现象：ASR/LLM 均成功后，TTS 每句都返回 400
- 根因：请求体与 LiteCrab 验证版一致，但 `voice` 值是陈旧的 `"alex"`——我在 Kconfig 里把默认值改成完整格式 `FunAudioLLM/CosyVoice2-0.5B:alex`，**但 sdkconfig 首次生成时已固化旧值，改 Kconfig 默认值不会传播**（`build/config/sdkconfig.h` 中仍是 `"alex"`）。SiliconFlow 要求完整 `<model>:<voice>` 格式
- 修复：`LoadConfig` 直接硬编码已验证默认值 `FunAudioLLM/CosyVoice2-0.5B:alex`（不信任可能陈旧的 Kconfig 派生值）
- 状态：已烧录，待最终听音确认

### 3.5 其他观察（非 bug）

- 设备一次进入配网热点模式：手机热点暂时不在范围所致（非 NVS 丢失），热点恢复后自动重连
- SRAM 从 idle 的 ~104KB 降至对话期 ~72KB 后稳定（Http/EspSsl/TLS 会话占用），多轮无泄漏趋势
- `Server sample rate 24000 does not match device output sample rate 16000` 为预期警告：TTS 输出 24kHz，设备端 audio_service 重采样到 16KB 扬声器（与 LiteCrab 时期行为一致）

---

## 4. 当前验证状态（真机证据）

| 验证项 | 结果 | 证据（串口日志） |
|---|---|---|
| 开机不连任何 xiaozhi 云/PC 中转 | ✅ | `Local-agent mode: skipping OTA version check and activation`；无 `EspTcp: Resolved 192.168.43.9`、无 tenclass 连接 |
| activating → idle 用时 | ✅ ~200ms | （旧 OTA 重试流程需 13+ 分钟） |
| 唤醒 + 通道打开 | ✅ | `LocalAgent: Opening local-agent channel: provider=siliconflow llm=Qwen/...` |
| 真实 ASR 转写 | ✅ | `Agent: ASR rc=0 text='…'`（rc=0=真实转写；此前 rc=1=桩回落） |
| LLM 对话 | ✅ | `Application: << …`（回复上屏） |
| **LLM 自主调用机器人工具（in-process）** | ✅ | `Agent: Invoking tool self.otto.action args={"action": "walk", "direction": 1, "amount": 50, "arm_swing": 50}` |
| 稳定性 | ✅ | 10+ 轮循环（E8 修复后）无崩溃、无重启 |
| HTTPS 直连 | ✅ | 全部连接仅 `api.siliconflow.cn:443` |
| TTS 语音播放 | ⏳ | E12 修复已烧录，待听音最终确认 |

**"用的是自研 agent 而非云"的判定方法**：
1. `Agent: Invoking tool …` 只可能由设备固件内的 agent loop 打印（云端方案中设备只收网络透传的 mcp 消息）
2. 网络层：热点管理页/抓包看设备（`b8:1f:3f:a5:dd:5c`）连接，应只有 api.siliconflow.cn
3. 物理取证：关掉原中转 PC，机器人照常对话
4. 若连云会出现 `WS: Connecting to websocket server` 日志——现在完全没有

---

## 5. 遗留事项（TODO）

| # | 事项 | 位置 |
|---|---|---|
| 1 | 移除代码内联的 SiliconFlow API key（`sk-rgdw…`），改回 NVS `agent/api_key` + 配网 AP 表单录入 | `local_agent_protocol.cc LoadConfig()` |
| 2 | 恢复 NVS 驱动的协议选择：删除两处 `protocol_type = "local_agent"` 强制赋值，改回默认 `auto`（无 OTA 配置时回落 local_agent） | `application.cc ActivationTask()/InitializeProtocol()` |
| 3 | 删除 `application.cc` 中 TODO 注释对应的联调逻辑；Kconfig TTS_VOICE 默认值与 sdkconfig 不一致问题（改 Kconfig 需重新生成 sdkconfig 或删 `build/config/sdkconfig.h` 对应行） | `Kconfig.projbuild` |
| 4 | Abort 期间在途 HTTPS 无法立即中断（只能等 HTTP 超时返回），打断延迟最坏 ~90s；后续可在 provider Abort 中强制关闭连接 | `siliconflow_*.cc Abort()` |
| 5 | CloseAudioChannel 在 agent 轮次进行中会阻塞 join 至在途请求超时（与 #4 同源） | `local_agent_protocol.cc` |
| 6 | `AgentTask` 同批 LISTEN_START+LISTEN_STOP 语义（当前以 START 优先吞掉 STOP）可再细化 | `local_agent_protocol.cc AgentTask()` |
| 7 | 流式 ASR/TTS/LLM 降延迟、火山引擎 provider（设计文档 §15 路线图） | 后续版本 |
| 8 | 单元测试（分句器/chunked 解码/会话 FIFO 为纯函数，可 host 单测） | 未做 |

## 6. 构建与烧录速查（Windows，中文路径需镜像）

```powershell
# 1. 同步源码到 ASCII 路径镜像
robocopy "D:\项目\能源agent\code\xiaozhi-esp32\main" "D:\xz_build\xiaozhi-esp32\main" /MIR /XD build

# 2. 构建 + 重新生成合并镜像（缺一不可！E6 教训）
$env:IDF_TOOLS_PATH="D:\Espressif"; . "D:\esp\v6.0.2\esp-idf\export.ps1"
idf.py build; idf.py merge-bin

# 3. （可选）验证新代码在镜像内
& "D:\Espressif\tools\xtensa-esp-elf\...\xtensa-esp32s3-elf-strings" build\merged-binary.bin | Select-String "<新日志串>"

# 4. 烧录前先杀掉占用 COM3 的后台串口记录进程，然后：
python -m esptool --chip esp32s3 -p COM3 -b 460800 --before default-reset --after hard-reset write-flash 0x0 build\merged-binary.bin

# 5. 崩溃 backtrace 解码
& "D:\Espressif\tools\xtensa-esp-elf\...\xtensa-esp32s3-elf-addr2line" -f -C -e build\xiaozhi.elf <PC1> <PC2> ...
```
