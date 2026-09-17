# xiaozhi-esp32 × LiteCrab agent 适配实施文档

| 项目 | 内容 |
|---|---|
| 日期 | 2026-09-14 ～ 2026-09-16 |
| 状态 | 已完成并通过端到端验收（mock e2e 7/7、WSL ctest 15/15、真实 SiliconFlow 全链路 PASS、真机已烧录进入配网） |
| 目标 | 将 LiteCrab agent（C11 单二进制 `litecrab_server`）作为 xiaozhi-esp32 设备的私有后端，替代 xiaozhi.me 官方云 |
| 相关仓库 | `xiaozhi-esp32`（本仓库，唯一真源）；上游 `LiteCrab_0829` 冻结不改 |

---

## 1. 总体架构（改造后的完整链路）

```
┌──────────────────┐  局域网 WebSocket (xiaozhi 协议)   ┌──────────────────────────────────────┐
│  xiaozhi-esp32    │ ─────────────────────────────────▶ │  服务器（WSL 开发 / 3516 部署）        │
│  otto-robot 机器人 │   opus 语音帧 / JSON 控制消息        │  litecrab_server（单二进制）           │
│  （固件零代码改动） │ ◀───────────────────────────────── │                                      │
│  屏幕+扬声器+舵机   │   文本 + 语音帧 + MCP 机器人指令     │  src/gateway/xiaozhi_ws.c   ← 新增    │
└──────────────────┘                                    │  src/gateway/ws_codec.c    ← 新增    │
         激活/报到 HTTP POST                             │  src/gateway/asr_sf.c      ← 新增    │
         CONFIG_OTA_URL ────────────────────────────────▶│  src/gateway/tts_sf.c      ← 新增    │
         （烧录时写入服务器 IP）                            │  src/kernel/kernel.c       ← 小改    │
                                                         │  src/config/config.c       ← 小改    │
                                                         │  src/main.c                ← 小改    │
                                                         │  CMakeLists.txt            ← 小改    │
                                                         │  （其余 LiteCrab 文件不动）             │
                                                         │            │                          │
                                                         │            ▼ HTTPS 直连（无中转）      │
                                                         │  云端 SiliconFlow（同一账号 key）：     │
                                                         │   ASR  /v1/audio/transcriptions      │
                                                         │   LLM  /v1/chat/completions          │
                                                         │   TTS  /v1/audio/speech              │
                                                         └──────────────────────────────────────┘
```

一轮完整对话（全部已实测）：

1. 设备开机 → HTTP POST 到 `CONFIG_OTA_URL` → 网关回答 `{"websocket":{"url":"ws://<服务器>:8000/xiaozhi/v1/","token":...}}`
2. 设备 WebSocket 握手（Bearer token）→ hello → 网关回 hello（声明下行音频 24kHz）
3. 网关自动执行 MCP 握手（`initialize` → `tools/list`），把设备的机器人工具（`self.otto.action` 等 8 个）注册进 LiteCrab kernel → LLM 从此可见可调
4. 用户说话 → 设备上传 opus 帧 → `listen stop` → 网关 libopus 解码 → WAV → SiliconFlow ASR → 转写文本
5. 文本经 `IngressSubmit` 进入 LiteCrab agent（规划/分析/Skill 调用）→ 如需控制机器人，LLM 调用 `self.otto.action` → 网关经 WS `tools/call` 下发设备执行 → 结果回传 LLM
6. agent 回复逐句下发：`tts start` → 每句 `sentence_start`（屏幕显示）+ SiliconFlow TTS 合成的 opus 音频帧（扬声器播放）→ `tts stop` → 下一轮

---

## 2. 代码仓变更总览

```
xiaozhi-esp32/
├── server/                          ★ 全部新增（184 个文件）
│   ├── README.md                    运维手册：构建/部署/烧录/网络/选型
│   ├── litecrab/                    LiteCrab 源码快照（唯一真源，上游冻结）
│   │   ├── src/gateway/
│   │   │   ├── xiaozhi_ws.c/h       ★ 新增 约1600行  xiaozhi WebSocket 网关（核心）
│   │   │   ├── ws_codec.c/h         ★ 新增 约350行   RFC6455 WebSocket 编解码
│   │   │   ├── asr_sf.c/h           ★ 新增 约650行   SiliconFlow ASR Provider
│   │   │   ├── tts_sf.c/h           ★ 新增 约450行   SiliconFlow TTS Provider
│   │   │   └── tcp_line.c           （原有，未动）      TCP 文本网关
│   │   ├── src/kernel/kernel.c      ✚ 修改 约40行     动态工具注册（设备工具注入 LLM）
│   │   ├── src/config/config.c      ✚ 修改 约60行     xiaozhi_ws 配置段解析
│   │   ├── src/main.c               ✚ 修改 约15行     启动/停止网关
│   │   ├── CMakeLists.txt           ✚ 修改 约40行     新源文件 + libopus 可选链接
│   │   ├── config/base_config.json  ✚ 新增/修改       运行时配置（key 内联入库）
│   │   ├── config/llm_config.json   ✚ 新增/修改       LLM 配置（key 内联入库）
│   │   ├── tests/test_ws.c          ★ 新增 约430行   143 项单元断言
│   │   ├── tests/opusenc_tool.c     ★ 新增 约90行    WAV→opus 帧编码工具（测试用）
│   │   └── scripts/fetch_opus_deb.sh ★ 新增          免 sudo 获取 libopus
│   └── tests/                       ★ 全部新增（PC 开发期端到端测试）
│       ├── run_e2e.py               e2e 运行器（7 场景）
│       ├── fake_device.py           纯标准库假设备（WS 客户端 + MCP 应答器）
│       ├── mock_llm.py              mock OpenAI chat/completions（含工具调用模拟）
│       ├── mock_asr.py              mock /v1/audio/transcriptions
│       └── mock_tts.py              mock /v1/audio/speech
├── docs/litecrab/
│   └── 2026-09-14-xiaozhi-litecrab-agent-design.md   设计文档（含 3 次修订记录）
├── sdkconfig.defaults               ✚ 修改 1 处：CONFIG_OTA_URL 指向 agent 服务器
├── build/                           固件产物（merged-binary.bin 10.66MB，含指向 agent 的报到地址）
└── main/ 等                        （固件零代码改动）
```

上游 `LiteCrab_0829` 仓库**完全未修改**（全部开发在本仓库 `server/litecrab` 内进行）。

---

## 3. 新增代码文件详解

### 3.1 `src/gateway/ws_codec.c / include/litecrab/ws_codec.h` — WebSocket 协议编解码

纯函数、与业务解耦，无第三方依赖：

| 功能 | 说明 |
|---|---|
| `WsParseHttpHead()` | 解析 HTTP 请求头：提取方法/路径/`Sec-WebSocket-Key`/`Authorization`/`Device-Id`/`Content-Length`，并判定是否 WebSocket 升级请求（同端口双路由的判定基础） |
| `WsComputeAccept()` | RFC6455 握手应答计算：`SHA1(key + 固定GUID)` → Base64（SHA-1 用 OpenSSL EVP 手写声明，无需 OpenSSL 头文件） |
| `WsParserFeed()` | 增量帧解析状态机：2/16/64 位长度、客户端强制掩码校验与去掩码、分片消息重组（控制帧可穿插）、消息上限保护 |
| `WsParserConsume()` | 允许继续解析下一条消息 |
| `WsEncodeFrame()` | 服务端发送侧帧编码（不掩码）：文本帧(JSON)/二进制帧(opus)/ping/pong/close |

### 3.2 `src/gateway/asr_sf.c / include/litecrab/asr_sf.h` — ASR Provider（用户语音 → 文本）

| 功能 | 说明 |
|---|---|
| Provider 接口 | `begin/feed_audio/finish/abort` 四回调（设计文档 §7.4 定义，阶段扩展点） |
| `siliconflow_provider`（默认） | 缓存 opus 帧 → libopus 逐帧解码为 PCM 16k/16bit/mono → 封 44 字节 WAV 头 → `multipart/form-data` POST `https://api.siliconflow.cn/v1/audio/transcriptions`（model=FunAudioLLM/SenseVoiceSmall）→ 解析 `{"text"}` |
| `stub_provider`（回落/联调） | 返回固定桩文本，免 key 跑测试 |
| 健壮性 | 自研 OpenSSL HTTPS 客户端（证书+主机名校验）；响应**截断检测**（Content-Length 不足即判失败）；**3 次重试 + 100ms 退避**（实测免费档排队 20–55s，偶发网络抖动由重试兜住）；chunked 编码解码 |
| 失败语义 | `finish` 返回 0=真实转写（可能空文本=用户没说话）/ 1=回落桩 / -1=硬失败 |

### 3.3 `src/gateway/tts_sf.c / include/litecrab/tts_sf.h` — TTS Provider（回复文本 → 语音）

| 功能 | 说明 |
|---|---|
| `TtsSfSynthesize()` | 逐句调用 `https://api.siliconflow.cn/v1/audio/speech`（model=FunAudioLLM/CosyVoice2-0.5B，voice=alex）→ 解析 WAV（24kHz/mono/16bit）→ **OpusEncoder 编码为 60ms 帧**（2 字节长度前缀，与设备解码参数一致） |
| 输出格式 | 长度前缀 opus 帧序列，网关逐帧作为 WebSocket 二进制帧下发 |
| 关键点 | SiliconFlow TTS 响应为 **chunked 传输编码**（curl 测试时被自动解码掩盖，C 客户端必须自己解码——已实现）；采样率 24000 写入 server hello 的 `audio_params`，设备端自动重采样到扬声器 |

### 3.4 `src/gateway/xiaozhi_ws.c / include/litecrab/xiaozhi_ws.h` — xiaozhi 网关（核心，约 1600 行）

这是整个适配的中枢，把设备协议与 LiteCrab agent 连接起来：

**连接与路由**
| 功能 | 说明 |
|---|---|
| 同端口双路由 | 一个 8000 端口：普通 HTTP POST → 激活检查点应答（`{"websocket":{url/token/version}}`，不含 mqtt 段→固件自动选 WebSocket 协议）；`GET+Upgrade` → WS 会话。**url 由 getsockname 自动探测**，报到通则对话必通 |
| worker 线程池 | 复制 tcp_line 模式：8 worker × 64 连接，每 worker 独占一条设备连接直至关闭 |
| 认证 | 握手校验 `Authorization: Bearer <token>`（token 由检查点下发给设备），失败 401 |
| 会话身份 | sessionId=`xiaozhi-<MAC>`（跨连接/重启恢复 .crab/sessions/ 会话），userId=`xiaozhi:<MAC>` |

**语音轮次状态机（IDLE→LISTENING→PROCESSING→IDLE）**
| 功能 | 说明 |
|---|---|
| 语音→ASR 必经 | `listen start` 缓存二进制帧，`listen stop` 触发 ASR 转写；空文本（没说话）静默回 IDLE 省一次 LLM |
| 回复下发 | ASR 文本→`stt`（屏显）→`llm`（表情）→ agent 回复按句切分（。！？!?；;\n，单句≤120 字符，UTF-8 不拆半字）→ 每句 `sentence_start` + TTS 音频帧 → `tts stop` |
| 打断 | PROCESSING 中收到 `abort` → `RequestCancel` 精确取消在途请求（等待线程 + 轮询 socket 的混合模型）；期间到达的新消息排队、本轮结束回放 |
| 超时/失败 | agent 超时→道歉文案；ASR 失败→桩文本 + 提示前缀；设备断开→清理全部资源并唤醒等待者 |

**MCP 设备工具桥（agent 下发机器人指令的关键）**
| 功能 | 说明 |
|---|---|
| 自动握手 | hello 后网关主动发 `initialize` → `tools/list`（支持 8000 字节分页 nextCursor），全流程异步状态机，不阻塞对话 |
| schema 转换 | 设备工具的 JSON Schema → `CrabToolParamSpec`（string/int/bool/数组、enum、min/max、required），调 `AgentLoopRegisterTool()` 注册进 kernel |
| 工具执行 | LLM 调用 `self.otto.action` → agent 线程把参数序列化为 JSON → WS `tools/call`（JSON-RPC）下发设备 → condvar 等待结果（20s 超时）→ 结果回填 LLM 继续推理。实测真实 LLM 自主调用 `self.otto.action({action:'walk',steps:3,direction:1,speed:500})` |
| 线程安全 | 三把锁：`g_sendMu`（所有设备 socket 写串行化——worker 与 agent 线程并发发送）、`g_mcpMu`+`g_mcpCv`（活跃会话指针/在途调用/应答唤醒）、kernel 的 `toolsMu` |
| 关键修复 | 轮次中收到 bootstrap 响应只排队不处理（注册会阻塞在 agent 的 LLM 锁上，曾导致 abort 延迟失效）；设备断开时唤醒所有在途调用 |

### 3.5 `tests/test_ws.c` — 单元测试（143 项断言）

覆盖：RFC6455 官方握手向量、帧编解码往返、分片重组+控制帧穿插、长度边界(125/126/65535/65536)、畸形帧拒绝、分句器（中英文标点/UTF-8 安全/120 字符硬切）、WAV 头字节快照、multipart 构造、转写响应解析、检查点 JSON（无 mqtt 段）、鉴权、配置校验、stub provider。并入 LiteCrab CTest。

### 3.6 `tests/opusenc_tool.c` — 测试音频工具

16kHz WAV → 长度前缀 opus 帧文件（OpusEncoder），e2e 用它把真实语音变成设备上行格式。

### 3.7 `scripts/fetch_opus_deb.sh` — libopus 免 sudo 获取

`apt-get download libopus-dev libopus0` + `dpkg -x` 解包到 `third_party/opus-deb/`（改 .gitignore 不入库），`LITECRAB_OPUS_PREFIX` 指向即可链接。**不动 WSL 系统环境**。

### 3.8 `server/tests/` — 端到端测试套件（纯 Python 标准库，Windows/WSL 双端可跑）

| 文件 | 功能 |
|---|---|
| `fake_device.py` | 假 xiaozhi 设备：纯标准库实现 WS 客户端（握手/masked 帧）、OTA 检查点、hello/listen/语音帧/chat/abort、**MCP 应答器**（模拟 otto 机器人：initialize/tools-list/tools-call 返回执行结果）、**音频帧收集器** |
| `mock_llm.py` | mock OpenAI 端点：skill resolver 应答 / 用户说"前进"→返回 `self.otto.action` tool_call / 工具结果→总结 / SLOW 延迟（测打断） |
| `mock_asr.py` | mock 转写端点：校验 WAV 合法性、返回固定文本（支持 MOCK_ASR_TEXT 覆盖） |
| `mock_tts.py` | mock 语音合成端点：按文本长度生成正弦 WAV |
| `run_e2e.py` | 一键编排：起 3 个 mock + 真实 server → 跑 7 场景 → 断言 → 清理。失败时保留现场（workspace + server 日志） |

7 个 e2e 场景：`checkpoint`（报到应答）/ `auth_reject`（401）/ `voice_turn`（语音→ASR→agent→TTS 音频）/ `text_chat`（文本直达不经 ASR）/ `empty_audio`（静默不触发）/ `voice_robot_command`（语音→LLM 调机器人工具→设备执行→总结→TTS）/ `abort_recover`（打断取消+下一轮恢复）。

---

## 4. 对 LiteCrab 原有代码的修改（全部在 server/litecrab 快照内，上游不动）

### 4.1 `src/kernel/kernel.c`（约 +40 行）—— 运行时工具注册

**问题**：kernel 在 `AgentLoopInit()` 时一次性渲染 tools JSON 到静态缓存（`toolsJson`/`baseToolsJson`），运行时注册的新工具 LLM 看不见。

**修改**：
```c
int AgentLoopRefreshTools(void);   // 持锁重渲染两个 tools JSON 缓存
int AgentLoopRegisterTool(const CrabToolSpec* spec);
   // 1) 注册进运行时 registry（重名视为成功——设备重连场景）
   // 2) 触发 AgentLoopRefreshTools()
   // 3) kernel.h 增加两个声明
```
配套：`run_round()` 每次 LLM 调用前后持/放 `toolsMu`（新增静态互斥量），保证注册与调用互斥。**agent 核心逻辑（调度/Skill/会话）零改动**。

### 4.2 `src/config/config.c` / `include/litecrab/config.h`（约 +60 行）—— 配置扩展

`LiteCrabAppConfig` 增加 `XiaozhiWsConfig xiaozhiWs`；`load_base()` 解析 `xiaozhi_ws` 段（enabled/listen_ip/port/advertised_ip/require_auth/auth_token/超时/并发 + 嵌套 `asr`、`tts` 子段）；`validate()` 增加：tcp 与 xiaozhi_ws 不得同址绑定、worker/连接上限、asr provider 合法性、require_auth 需配 token 等；环境变量 `LITECRAB_XIAOZHI_WS_AUTH_TOKEN`/`LITECRAB_XIAOZHI_WS_PORT` 覆盖。

### 4.3 `src/main.c`（约 +15 行）—— 启动接入

`xiaozhi_ws.enabled` 时调用 `XiaozhiWsStart()`（独立监听线程，与 tcp_line 并存）；信号处理和退出路径接入 `XiaozhiWsRequestStop()`/`XiaozhiWsStop()`。

### 4.4 `CMakeLists.txt`（约 +40 行）

- `litecrab` 库新增 4 个源文件
- **libopus 可选探测**（`LITECRAB_OPUS_PREFIX` 提示，找不到降级为 stub-only 并告警，不破坏原有构建）
- 静态优先链接 libopus.a（动态 .so 的 RPATH 会破坏单二进制部署契约）
- 新测试目标 `litecrab_ws_tests`、工具目标 `litecrab_opusenc`

### 4.5 `config/*.json` — 运行时配置（key 按项目要求直接内联入库）

`base_config.json`：xiaozhi_ws 全配置（ASR/TTS/MCP 均默认开启，key 内联）；`base_config.example.json`：不含真实 key 的模板；`llm_config.json`：default=siliconflow 直连（`Qwen/Qwen3.5-35B-A3B`，实测 2s；保留 GLM/Qwen122B 备选条目及实测结论注释）。

---

## 5. 固件侧改动与真机调试补丁

```diff
# sdkconfig.defaults（当前 Windows 热点地址）
+ CONFIG_OTA_URL="http://192.168.43.9:8000/"
```

设备开机联网后向此地址报到，agent 自答"对话服务器就是我"（检查点与对话同一端口双路由），之后所有语音/文本/MCP 流量全部走你的 agent。服务器迁移（如部署 3516）仍只需更新报到地址并重编。

真机联调后增加了三处必要固件修复：

- `main/boards/otto-robot/otto_robot.cc`：用 GPIO15/GPIO16 稳定电平识别硬件版本，避免摄像头探测失败连带选错 LCD/音频引脚，修复烧录后黑屏。
- `main/application.{cc,h}`：自动聆听时跟踪本地 VAD 的“说话→静音”边沿并主动提交，修复用户说完后设备长期不结束一轮。
- `main/application.cc`：网络 TTS 包调用 `PushPacketToDecodeQueue(..., true)`。解码队列只有约 1.2 秒容量；原默认 `wait=false` 会在 24kHz→16kHz 重采样短时落后时静默丢弃 Opus 包，造成卡顿、缺字和截断。改为等待后由 TCP/WebSocket 自然背压，保证帧完整有序。

服务端 `xiaozhi_ws.c` 不再按 60ms 定时休眠发送每个 Opus 包，而是先快速填充设备缓冲区，再由上述背压限制发送速度，避免操作系统调度抖动直接形成播放欠载。

已验证烧入二进制：`build/merged-binary.bin`（11,178,322 bytes，含 bootloader+分区表+app+assets）；2026-09-16 最新防丢帧版本 SHA-256 为 `371DA17E75DB03DA226261ADB58FE3D66C1CB4C04BE1ACFA0E5DFC2C48583C0D`。

---

## 6. 为"用上 agent"所做的关键适配决策（为什么这么改）

| 适配点 | 决策与理由 |
|---|---|
| agent 不能跑在 ESP32 上 | LiteCrab 依赖 POSIX 进程/线程/TLS/文件系统；ESP32 是 MCU。agent 部署在 Linux 侧（WSL 开发→3516 部署），设备保持"语音/显示/执行终端"角色 |
| 设备固件协议不可协商 | 网关严格按 `docs/websocket.md` 实现 hello/listen/stt/llm/tts/abort/mcp 与 opus 二进制帧——对固件来说 agent 就是"云" |
| 官方云报到机制 | 网关同端口双路由：HTTP POST 答 `{"websocket":{...}}`（无 mqtt 段→固件选 WebSocket），url 自动探测本机地址 |
| LLM 看不到设备工具 | kernel 静态 tools 缓存 → 新增运行时注册+重渲染（§4.1），设备连接即注入 `self.otto.*` |
| LLM 内部循环/工具结果不经 ASR | ASR 只在"设备语音输入侧"（网关 listen stop 触发），agent 内部消息流（skill 路由/工具结果/多轮推理）与 ASR 物理隔离 |
| 回复要能听 | TTS 逐句合成+libopus 编码 60ms 帧，`audio_params.sample_rate=24000` 声明，设备自动重采样（固件 audio_service 原生支持） |
| 直连硅基流动（无中转） | asr_sf/tts_sf/llm.c 三条 HTTPS 链路均直连 `api.siliconflow.cn`，LiteCrab 的 transfer_station/relayhub 等中转组件完全未用 |
| key 管理方式 | 按项目要求直接内联写入 `config/*.json` 并入库（生产环境建议改回 apiKeyEnv 方式） |
| 中文路径无法编译固件 | xtensa 工具链 + idf.py realpath 硬限制 → 镜像到 `D:\xz_build`（ASCII 路径）编译，产物拷回仓库 `build/` |
| WSL NAT 隔离 | portproxy + 防火墙放行（管理员执行一次），局域网设备经 Windows IP 访问 WSL 里的 agent；3516 部署后不需要 |

---

## 7. 验证记录（全部本地完成）

| 验证项 | 结果 | 说明 |
|---|---|---|
| WSL ctest | **15/15** | 含 143 项新断言 + LiteCrab 原有全部测试无回归 |
| mock e2e | **7/7**（多轮稳定） | 7 场景见 §3.8 |
| 真实 SiliconFlow 全链路 ×2 | **PASS** | 真实英语语音→ASR 精确转写→真实 LLM 自主调用 `self.otto.action(walk,3)`→设备执行→真实 TTS 34–70 帧音频；全链路 20–58s（ASR 免费档排队为主） |
| Windows 侧验收（设备网络位置） | **6/6** | 检查点/401/MCP/语音机器人全链路/文本直达/打断恢复 |
| 固件编译 | **通过** | `python scripts/build.py otto-robot` + 自动 merge-bin；二进制含新报到地址、官方云地址已清除 |
| 真机 | **已烧录** | 全擦+完整写入 11.18MB，Hash 校验通过；启动日志确认屏幕点亮、8 个机器人 MCP 工具注册、进入配网模式（热点 Xiaozhi-DD5D） |

**已知事项**：mock e2e 的 voice_turn 存在约 1/10 概率因 WSL localhost 抖动回落桩文本（有 3 次重试+退避+回落三重兜底；真实外网 API 不经 localhost，无此问题）；SiliconFlow ASR 免费档排队 20–55s 属正常（超时设 90s）。

---

## 8. 运行/部署速查

```sh
# WSL 开发环境构建与测试
cd server/litecrab
sh scripts/fetch_opus_deb.sh                    # 首次
export LITECRAB_OPUS_PREFIX=$PWD/third_party/opus-deb
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release && cmake --build build-wsl -j
cd build-wsl && ctest                           # 15/15
cd .. && python3 tests/run_e2e.py \             # 7/7
  litecrab/build-wsl/litecrab_server litecrab/build-wsl/litecrab_opusenc

# 启动 agent（生产配置）
./build-wsl/litecrab_server --config config/base_config.json \
  --llm-config config/llm_config.json --workspace .

# 固件编译（中文路径限制→镜像）
robocopy "D:\项目\能源agent\code\xiaozhi-esp32" "D:\xz_build\xiaozhi-esp32" /MIR /XD build build-wsl .git logs .crab third_party __pycache__
cd D:\xz_build\xiaozhi-esp32; $env:IDF_TOOLS_PATH="D:\Espressif"; . D:\esp\v6.0.2\esp-idf\export.ps1
python scripts/build.py otto-robot              # 自动 build + merge-bin

# 烧录（COM3 = 机器人 USB-JTAG）
python -m esptool --chip esp32s3 -p COM3 -b 460800 --before default-reset --after hard-reset write-flash 0x0 build\merged-binary.bin

# 局域网放行（管理员，一次性）
netsh interface portproxy add v4tov4 listenport=8000 listenaddress=0.0.0.0 connectport=8000 connectaddress=<WSL-IP>
netsh advfirewall firewall add rule name="xiaozhi-agent-8000" dir=in action=allow protocol=TCP localport=8000
```

3516 部署、LLM/TTS 选型实测结论、故障排查详见 `server/README.md`；协议与设计细节见 `docs/litecrab/2026-09-14-xiaozhi-litecrab-agent-design.md`（含 3 次修订记录）。

---

## 9. 2026-09-16 真机适配问题修复明细

本章记录 LiteCrab agent 接入 Otto/xiaozhi-esp32 真机后，为解决黑屏、无法连接本机 agent、说完不提交以及 TTS 卡顿/丢帧所做的实际修改。这里记录的是最终保留方案，也说明调试过程中尝试过但未单独解决问题的方案。

### 9.1 调试环境与最终基线

| 项目 | 最终使用值 |
|---|---|
| 真机芯片 | ESP32-S3 QFN56 rev 0.2，16MB Flash，8MB PSRAM |
| USB 串口 | `COM3`，USB Serial/JTAG |
| 固件板型 | `otto-robot` |
| ESP-IDF | `D:\esp\v6.0.2\esp-idf`，版本 6.0.2 |
| 工具目录 | `D:\Espressif` |
| 源码目录 | `D:\项目\能源agent\code\xiaozhi-esp32` |
| ASCII 编译镜像 | `D:\xz_build\xiaozhi-esp32` |
| Wi-Fi | `电子西兰花` |
| Windows/agent 地址 | `192.168.43.9` |
| 机器人地址 | `192.168.43.8` |
| Agent/OTA/WS 入口 | `http://192.168.43.9:8000/` / `ws://192.168.43.9:8000/xiaozhi/v1/` |

用户最初指定的 `D:\esp\v5.5.5` 环境没有形成一套可直接完成本项目构建的有效工具链；项目现有依赖和已验证构建实际使用 ESP-IDF 6.0.2。因此本轮所有最终固件均以 6.0.2 编译。源码所在路径包含中文，Xtensa/CMake 工具链在该路径下存在兼容性问题，所以只把 `D:\xz_build\xiaozhi-esp32` 当作编译镜像，权威源码仍保存在仓库目录中。

### 9.2 问题一：烧录后屏幕不亮

#### 现象

- 固件可以烧录，ESP32-S3 也能启动，但 LCD 背光和界面不正常。
- 原自动检测没有检测到摄像头，于是整块板被判定为“无摄像头版”。
- Otto 的摄像头版与无摄像头版不仅摄像头不同，LCD、I2S 音频和舵机 GPIO 也完全不同。错误板型会直接初始化错误引脚。

关键差异如下：

| 资源 | 摄像头版 | 无摄像头版 |
|---|---|---|
| LCD 背光 | GPIO38 | GPIO3 |
| LCD MOSI/CLK | GPIO45/GPIO48 | GPIO10/GPIO9 |
| LCD DC/RST | GPIO47/GPIO1 | GPIO46/GPIO11 |
| 音频输出采样率 | 16000 | 24000 |
| 音频结构 | 共用 I2S，GPIO39/40/41/42 | 分离式 I2S，GPIO4/5/6/7/15/16 |

#### 根因

`main/boards/otto-robot/otto_robot.cc` 原来的 `DetectHardwareVersion()` 同时承担两项职责：

1. 判断 PCB/板型是摄像头版还是无摄像头版；
2. 通过 I2C 探测摄像头传感器及型号。

只要摄像头本体未安装、供电异常或 I2C 没有响应，函数就返回 `false`，随后选择 `NON_CAMERA_VERSION_CONFIG`。这把“摄像头当前是否可用”和“PCB 应使用哪组引脚”错误地绑定在一起。

#### 修改文件：`main/boards/otto-robot/otto_robot.cc`

具体修改：

1. 增加 `#include <esp_rom_sys.h>`，用于微秒级稳定采样延迟。
2. 增加成员 `bool is_camera_board_`，将“板型”与已有的 `has_camera_` 分开保存。
3. 重写 `DetectHardwareVersion()`：
   - 将 GPIO15、GPIO16 配为输入和内部下拉；
   - 等待 5ms 电平稳定；
   - 连续采样 8 次，每次间隔 1ms；
   - 两个引脚全部持续为高才判定为摄像头版；任一次为低则判定为无摄像头版。
4. 将原摄像头 I2C 探测逻辑拆成独立的 `DetectCamera()`。
5. 构造阶段先根据 `is_camera_board_` 选择 `CAMERA_VERSION_CONFIG` 或 `NON_CAMERA_VERSION_CONFIG`，再单独探测摄像头。
6. 摄像头版没有探测到摄像头时只记录警告并跳过摄像头初始化，不再退回另一套 LCD/音频引脚。

最终关系变为：

```text
GPIO15/GPIO16 板型识别
        |
        +-- 摄像头版 PCB --> 使用摄像头版 LCD/音频/舵机引脚
        |                       |
        |                       +-- I2C 探测到摄像头：初始化摄像头
        |                       +-- 未探测到摄像头：仅禁用摄像头
        |
        +-- 无摄像头版 PCB --> 使用无摄像头版 LCD/音频/舵机引脚
```

#### 解决效果

串口启动日志确认：

```text
板型识别: GPIO15/GPIO16 稳定为高，判定为摄像头版
自动检测硬件版本: 摄像头版
摄像头版未检测到摄像头，将跳过摄像头初始化
LcdDisplay: Turning display on
LcdDisplay: Initialize LVGL library
Backlight: Set brightness to 75
```

即使摄像头没有响应，屏幕和音频仍使用正确的摄像头版硬件配置，黑屏问题随之解决。

### 9.3 问题二：机器人无法连接 `huawei-guest`，更换热点后仍找不到 agent

#### 现象与网络拓扑

机器人无法使用 `huawei-guest` 后，电脑和机器人都切换到热点 `电子西兰花`。热点分配结果为：

```text
Windows：192.168.43.9
机器人：192.168.43.8
网关：  192.168.43.1
```

LiteCrab 运行在 WSL 中。机器人能访问 Windows 局域网地址，但不能直接访问 WSL 的 localhost；同时服务端返回给机器人的 WebSocket 地址必须是机器人可达的 Windows 地址，不能是 `127.0.0.1` 或旧网络地址。

#### 修改文件：`sdkconfig.defaults`

增加并更新固件报到地址：

```ini
CONFIG_OTA_URL="http://192.168.43.9:8000/"
```

设备启动后先请求该地址的检查点接口，LiteCrab 再返回同一台主机上的 WebSocket 地址。

#### 修改文件：`server/litecrab/config/base_config.json`

将 xiaozhi 网关配置为监听 8000，并向设备公布 Windows 热点地址：

```json
"xiaozhi_ws": {
  "enabled": true,
  "listen_ip": "0.0.0.0",
  "listen_port": 8000,
  "advertised_ip": "192.168.43.9"
}
```

配置文件中的鉴权 token 和第三方 API key 不应复制到公开文档或日志；这里只记录与网络适配有关的非敏感字段。

#### 新增文件：`server/scripts/windows_tcp_proxy.py`

新增 asyncio 双向 TCP 桥接程序，用于在没有管理员权限或 Windows `portproxy` 不可用时，将机器人访问的 Windows 地址转发到 WSL/localhost 中的 LiteCrab：

```text
192.168.43.9:8000  <-->  127.0.0.1:8000
```

脚本的主要行为：

- `asyncio.start_server()` 监听指定 Windows LAN 地址和端口；
- 为每个客户端创建到目标地址的上游 TCP 连接；
- 两个 `relay()` 协程同时转发上下行字节流；
- 正确处理连接关闭和取消，不解析也不改写 HTTP/WebSocket 数据。

#### 解决效果

设备日志确认机器人接入热点、获取地址并完成检查点访问：

```text
WifiStation: Got IP: 192.168.43.8
Application: Network connected
EspTcp: Resolved 192.168.43.9 -> 192.168.43.9
HttpClient: Established new connection to 192.168.43.9:8000
Application: Activation done
```

唤醒后进一步确认：

```text
WS: Connecting to websocket server: ws://192.168.43.9:8000/xiaozhi/v1/
WebSocket: WebSocket handshake done
```

### 9.4 问题三：用户已经说完，但机器人一直不提交

#### 根因

LiteCrab 在收到 `listen/stop` 后才把本轮 Opus 音频交给 ASR。xiaozhi 原自动模式主要假设服务端 VAD 会决定结束时间，而当前 LiteCrab 适配选择由设备明确提供一轮语音边界。因此设备虽然持续上传音频，LiteCrab 却一直等不到结束事件，表现为用户反复说“我已经说完了”仍没有回复。

#### 修改文件：`main/application.h`

新增两个状态字段：

```cpp
bool auto_stop_speech_detected_ = false;
int64_t listening_started_at_us_ = 0;
```

- `auto_stop_speech_detected_`：记录本轮是否已经检测到真实说话；
- `listening_started_at_us_`：记录进入聆听状态的时间。

#### 修改文件：`main/application.cc`

在进入 `kDeviceStateListening` 时重置本轮 VAD 状态并保存起始时间。在主事件循环处理 VAD 变化时，仅对 `kListeningModeAutoStop` 执行以下状态机：

```text
进入聆听
  -> 忽略前 500ms（避免无 AEC 板把唤醒提示音当成用户语音）
  -> IsVoiceDetected() == true：标记本轮已经有人说话
  -> 已说话后 IsVoiceDetected() == false：调用 HandleStopListeningEvent()
  -> 固件发送 listen/stop，LiteCrab 开始 ASR
```

该实现不会在从未检测到语音时因环境瞬时静音而提交空请求，也不会改变手动聆听模式。

#### 解决效果

串口可稳定出现：

```text
Application: Wake word detected
StateMachine: State: connecting -> listening
Application: Local VAD detected end of speech, submitting turn
StateMachine: State: listening -> idle
```

真实测试中只说一次“介绍一下你自己”即被识别并进入回复流程，不再要求用户重复说明已经说完。

### 9.5 问题四：机器人有回复，但语音卡顿、缺字和截断

#### 根因分析

服务端生成 24kHz、60ms 一帧的 Opus，摄像头版 Otto 音频输出为 16kHz，因此设备需要边解码边做 24kHz→16kHz 重采样。固件播放链路为：

```text
WebSocket Opus 包
  -> audio_decode_queue_（最多 1200/60 = 20 包，约 1.2 秒）
  -> Opus 解码/重采样
  -> audio_playback_queue_（最多 2 个任务）
  -> I2S 扬声器
```

`AudioService::PushPacketToDecodeQueue()` 的参数默认是 `wait=false`。队列满时该函数直接返回 `false`：

```cpp
if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
    if (wait) {
        // 等待队列出现空位
    } else {
        return false;  // 原网络 TTS 路径在这里静默丢帧
    }
}
```

原 `OnIncomingAudio` 没有检查返回值，也没有传 `wait=true`。当网络到包速度暂时快于解码/重采样时，超过 1.2 秒缓冲容量的包被静默丢弃，听感就是卡顿、句子缺失或尾部不完整。

#### 修改文件：`main/application.cc`

将网络 TTS 入队从：

```cpp
audio_service_.PushPacketToDecodeQueue(std::move(packet));
```

改为：

```cpp
audio_service_.PushPacketToDecodeQueue(std::move(packet), true);
```

队列满时 WebSocket 接收线程现在会等待解码队列出现空位。等待会沿 TCP 接收窗口逐级形成背压，服务端不能无限快地继续灌入数据，因此每个 Opus 包都会按顺序保留。播放中止或 decoder generation 改变时，原 `PushPacketToDecodeQueue()` 实现会唤醒等待者并安全返回，不会导致永久阻塞。

这一选择也与项目已有的通知音和 Ogg 播放路径一致；它们本来就使用 `PushPacketToDecodeQueue(..., true)` 保证音频完整。

#### 修改文件：`server/litecrab/include/litecrab/tts_sf.h`

新增：

```c
int TtsSfFrameMs(void);
```

让 WebSocket 网关读取 TTS 当前实际帧时长，不再在协议 hello 和日志中重复硬编码 `60`。

#### 修改文件：`server/litecrab/src/gateway/tts_sf.c`

实现 `TtsSfFrameMs()`，返回初始化后经过合法性校验的 `g_tts.frameMs`。当前允许 20、40、60ms，非法配置回退为 60ms。

#### 修改文件：`server/litecrab/src/gateway/xiaozhi_ws.c`

具体修改：

1. WebSocket `hello.audio_params.frame_duration` 改为调用 `TtsSfFrameMs()`，确保协议声明与实际编码一致。
2. 每个 TTS 句子发送后记录帧数和对应音频时长：

   ```text
   tts sent frames=<帧数> duration_ms=<帧数×frameMs> text=<句子前60字符>
   ```

3. 调试中曾先尝试每发送一帧后 `nanosleep(frameMs)`，希望用服务端实时限速避免队列溢出。真机反馈仍有卡顿，因为严格 60ms 一包没有启动预缓冲，Windows/WSL 调度或网络的微小延迟会直接导致播放队列欠载。
4. 最终移除逐帧 `nanosleep`：服务端先快速填充设备约 1.2 秒的解码缓冲，缓冲满后由设备端 `wait=true` 和 TCP/WebSocket 背压自动限制速度。这样同时避免“发太快溢出”和“发得刚好但因调度抖动断流”。

最终数据流为：

```text
LiteCrab 快速发送开头若干 Opus 帧
        |
        v
ESP32 建立约 1.2 秒预缓冲并连续播放
        |
        v
队列满时 PushPacketToDecodeQueue(wait=true) 等待
        |
        v
TCP 接收窗口收缩，服务端发送自然减速
        |
        v
队列释放后继续接收，所有帧保持顺序且不丢弃
```

### 9.6 本轮修改文件汇总

| 文件 | 修改内容 | 解决的问题 |
|---|---|---|
| `main/boards/otto-robot/otto_robot.cc` | 分离板型检测与摄像头探测；GPIO15/16 稳定采样；摄像头缺失时保持正确板型 | 黑屏、LCD/音频引脚选择错误 |
| `main/application.h` | 增加自动停止所需的语音检测和聆听起始状态 | 记录一轮语音边界 |
| `main/application.cc` | 本地 VAD 检测“说话→静音”并提交；网络 TTS 使用阻塞可靠入队 | 说完不回复；TTS 丢帧/截断 |
| `sdkconfig.defaults` | OTA/检查点 URL 改为 `192.168.43.9:8000` | 机器人找到本机 LiteCrab |
| `server/litecrab/config/base_config.json` | `advertised_ip` 改为 `192.168.43.9`，保留 8000 端口和 TTS 24kHz/60ms 参数 | 返回机器人可达的 WebSocket 地址 |
| `server/scripts/windows_tcp_proxy.py` | 新增 Windows LAN 到 WSL/localhost 的双向 TCP 桥 | 绕过 WSL NAT/管理员 portproxy 限制 |
| `server/litecrab/include/litecrab/tts_sf.h` | 声明 `TtsSfFrameMs()` | 统一 TTS 帧参数来源 |
| `server/litecrab/src/gateway/tts_sf.c` | 返回实际 TTS 帧时长 | 协议声明和编码配置一致 |
| `server/litecrab/src/gateway/xiaozhi_ws.c` | hello 使用实际 frame duration；增加帧统计；采用预缓冲+设备背压 | 降低抖动并便于核对回复是否完整发送 |
| `docs/litecrab/2026-09-16-adaptation-implementation-guide.md` | 更新当前网络、固件变更、产物信息并追加本章 | 保留可复现的实施记录 |

`D:\xz_build\xiaozhi-esp32\main\...` 中的同名修改仅是为了绕过中文路径构建限制而同步的编译镜像，不是另一套源代码；后续修改应先落到仓库，再同步到该目录。

### 9.7 编译、烧录和验证记录

#### 固件构建

```powershell
$env:IDF_TOOLS_PATH='D:\Espressif'
. 'D:\esp\v6.0.2\esp-idf\export.ps1'
python scripts/build.py otto-robot
```

结果：

- `xiaozhi.bin` 大小：`0x2c3ca0` bytes；
- 最小应用分区：`0x3f0000` bytes；
- 剩余空间：约 30%；
- `merged-binary.bin` 大小：11,178,322 bytes；
- SHA-256：`371DA17E75DB03DA226261ADB58FE3D66C1CB4C04BE1ACFA0E5DFC2C48583C0D`。

#### USB 烧录

```powershell
idf.py -p COM3 -b 460800 flash
```

实际写入 bootloader、partition table、OTA data、assets 和 application，各分区写入后均通过 Hash 校验，最后由 RTS 硬复位。

#### 服务端构建与测试

```sh
cd /mnt/d/项目/能源agent/code/xiaozhi-esp32/server/litecrab
cmake --build build-wsl -j2
ctest --test-dir build-wsl --output-on-failure
```

LiteCrab 完整测试在本轮适配中通过 `15/15`；取消逐帧 sleep 后再次完成增量编译和链接，无编译错误。

#### 真机链路验证

已确认以下完整路径：

```text
唤醒词
 -> ws://192.168.43.9:8000/xiaozhi/v1/ 握手
 -> listening
 -> 本地 VAD 自动提交
 -> ASR 识别“介绍一下你自己”
 -> speaking
 -> 24kHz Opus 在设备侧重采样为 16kHz
 -> 播放回复
```

验证时 ESP32 最低内部可用 SRAM 约 78KB，没有发生看门狗复位、崩溃或内存耗尽。

### 9.8 当前仍需区分的非丢帧问题

1. **ASR 响应慢**：SiliconFlow ASR 曾出现读取等待和重试，真实请求从提交到开始回复约 52 秒，极端情况可接近 90 秒超时。这会造成“很久才回答”，但不是 TTS 播放卡顿。
2. **采样率重采样**：服务端 TTS 为 24kHz，摄像头版扬声器输出为 16kHz，串口会提示 `resampling may cause distortion`。当前固件能完成重采样；若仍有音色失真，应优先让 TTS 直接输出 16kHz或评估板端输出 24kHz，而不是重新放开丢包。
3. **摄像头缺失**：板型已正确识别，但物理摄像头仍未探测到。现在只影响摄像头功能，不再影响屏幕和音频。
4. **串口终端编码**：回复含 emoji 时，Windows 默认 GBK 的 `idf.py monitor` 可能因 `UnicodeEncodeError` 退出。该错误发生在电脑监视器进程，不代表 ESP32 重启；调试时可先设置 `PYTHONIOENCODING=utf-8` 或使用 UTF-8 终端。
5. **热点地址可能变化**：手机热点重新分配 Windows IP 后，需要同步更新 `sdkconfig.defaults` 的 `CONFIG_OTA_URL` 和 `base_config.json` 的 `advertised_ip`，然后重新编译烧录固件。

### 9.9 后续维护原则

- 不要再用“是否探测到摄像头”决定整个 Otto 板型；板型和外设存在性必须独立判断。
- 网络 TTS 不应恢复默认的非阻塞入队；若调用方必须使用 `wait=false`，必须显式处理失败并设计重传，否则会再次出现静默丢帧。
- 不要只靠服务器逐帧定时发送来控制播放速度；设备端缓冲和背压才是可靠性边界。
- 修改 TTS `frame_ms` 时，要同时保证 Opus 编码帧、WebSocket hello 声明和设备解码逻辑一致。
- 每次更换网络或部署主机，都要从机器人所在网络验证检查点 URL 和返回的 WebSocket URL 均可达。
- 发布固件时同时记录构建所用 IDF 版本、板型、二进制大小和 SHA-256，避免网页烧录到旧产物。
