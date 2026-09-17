# xiaozhi-esp32 本地 Agent 服务端（LiteCrab + xiaozhi 网关）

xiaozhi-esp32 设备的私有后端：设备通过 WebSocket 上传语音，网关调用
SiliconFlow ASR（SenseVoiceSmall）转写为文本后交给 LiteCrab agent 规划分析，
回复以**文本 + 语音（CosyVoice2 TTS → Opus 音频帧）**形式下发到设备屏幕和
扬声器；agent 可调用**设备端 MCP 工具**（如 otto 机器人的 walk/turn/jump 动作
指令），像 xiaozhi-esp32-server 一样驱动机器人。非语音消息（`type=chat` 文本）
直接进入 agent，不经过 ASR；LLM 内部循环与工具结果同样不经过 ASR。

```
┌──────────────┐  局域网 WebSocket   ┌──────────────────────────┐   HTTPS   ┌─────────────┐
│ xiaozhi 设备  │ ─────────────────▶ │ 3516 板 litecrab_server  │ ────────▶ │ SiliconFlow │
│ (otto-robot) │   opus 语音/JSON    │  xiaozhi_ws 网关 + agent  │           │ ASR/LLM/TTS │
│              │ ◀───────────────── │                          │ ◀──────── │             │
│ 屏幕+扬声器   │  文本+语音+MCP指令   │  设备MCP工具注册给LLM      │           └─────────────┘
└──────────────┘                    └──────────────────────────┘
```

设计文档见 `docs/litecrab/2026-09-14-xiaozhi-litecrab-agent-design.md`。

## 目录结构

```
server/
├── README.md            # 本文件
├── litecrab/            # 服务端源码（唯一真源，LiteCrab_0829 不再直接修改）
│   ├── src/gateway/xiaozhi_ws.c   # xiaozhi WebSocket 网关（同端口双路由）
│   ├── src/gateway/ws_codec.c     # RFC6455 帧编解码 + 握手
│   ├── src/gateway/asr_sf.c       # SiliconFlow ASR Provider（opus→WAV→multipart）
│   ├── scripts/fetch_opus_deb.sh  # 免 sudo 获取 libopus（解码依赖）
│   ├── config/base_config.json    # 运行时配置（API key 已按需内联入库）
│   ├── config/llm_config.json     # LLM 配置（key 内联入库）
│   └── tests/…                    # 单元测试 + opusenc 工具
└── tests/               # PC 开发期端到端测试（纯 Python 标准库）
    ├── run_e2e.py       # e2e 运行器（起 mock + 服务端 + 假设备）
    ├── fake_device.py   # 假 xiaozhi 设备（纯标准库 WebSocket 客户端）
    ├── mock_llm.py      # mock OpenAI 兼容 chat/completions
    └── mock_asr.py      # mock /v1/audio/transcriptions
```

## 关键行为

- **一个端口双路由**（默认 8000）：`GET + Upgrade: websocket` 走 xiaozhi WS 会话；
  其余 HTTP POST（设备激活检查点）返回 `{"websocket":{"url","token","version"}}`，
  不含 `mqtt` 段 → 固件自动选择 WebSocket 协议。
- **语音轮次（ASR 必经）**：`listen start` → 缓存 opus 帧 → `listen stop` →
  libopus 解码 → WAV → SiliconFlow ASR → `stt` 文本 → agent（Ingress）→
  `tts` 分句下发 → 下一轮。LLM 内部循环、工具结果不经过 ASR。
- **非语音轮次**：`{"type":"chat","text":"..."}` 直接 `stt` → agent，不经过 ASR。
- **设备 MCP 工具桥（机器人指令）**：设备连接后网关自动执行 MCP 握手
  （`initialize` → `tools/list` 分页拉取），把设备工具（如 `self.otto.action`）
  转换为 agent 工具注册进 LiteCrab kernel——LLM 看到并可调用；调用时通过
  WebSocket `tools/call` JSON-RPC 下发设备执行并等待结果（20s 超时）。
- **TTS 语音回复**：每句回复经 SiliconFlow CosyVoice2（24kHz wav）合成，
  libopus 编码为 60ms 帧后随 `sentence_start` 后的二进制帧下发；server hello
  的 `audio_params.sample_rate` 自动声明 24000，设备端自动重采样播放。
  TTS 失败时自动降级为纯文本。
- **失败回落**：ASR 失败/超时回桩文本（`asr.fallback_to_stub`，自动重试 3 次）；
  agent 超时/失败发道歉文案，设备不会停在 listening。
- **打断**：处理期间收到 `abort` → `RequestCancel` 取消在途请求，不发回复；
  处理期间到达的新消息排队，本轮结束后回放。
- **会话持久**：sessionId=`xiaozhi-<设备MAC>`，跨连接/重启恢复
  （`<workspace>/.crab/sessions/`）。

## WSL 构建（开发验证环境）

```sh
cd server/litecrab
sh scripts/fetch_opus_deb.sh        # 免 sudo 获取 libopus（首次）
export LITECRAB_OPUS_PREFIX=$PWD/third_party/opus-deb
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release
cmake --build build-wsl -j
cd build-wsl && ctest               # 15 项测试（含 ws 网关单测）
```

## 端到端测试

mock 版（不消耗真实 API）：

```sh
cd server
python3 tests/run_e2e.py litecrab/build-wsl/litecrab_server litecrab/build-wsl/litecrab_opusenc
# 预期 7/7：checkpoint / auth_reject / voice_turn / text_chat / empty_audio /
#           voice_robot_command（语音→ASR→LLM调设备工具→执行→TTS音频）/ abort_recover
```

真实 API 版（`tests/run_e2e.py` 换真实 endpoint，或参考下方实测）。

**真实 SiliconFlow 全链路实测**（2026-09-15，WSL）：
- 输入：英语语音 "Please walk forward three steps."
- 真实 ASR（SenseVoiceSmall，免费档排队 20–55s）：转写精确
- 真实 LLM（Qwen3.5-35B-A3B）自主调用设备工具：
  `self.otto.action {action:"walk", steps:3, direction:1, speed:500}`
- 设备执行后 LLM 总结，真实 TTS（CosyVoice2）34–70 帧 opus 音频随句下发
- 全链路 20–55s（ASR 排队为主），PASS

## 运行（WSL / 3516 通用）

```sh
cd server/litecrab
./build-wsl/litecrab_server \
  --config config/base_config.json \
  --llm-config config/llm_config.json \
  --workspace .
```

配置说明（`config/base_config.json`，完整键见设计文档 §8）：

| 键 | 说明 |
|---|---|
| `xiaozhi_ws.enabled` | 总开关（默认已开） |
| `xiaozhi_ws.listen_port` | 设备连接端口（默认 8000） |
| `xiaozhi_ws.auth_token` | 设备握手 Bearer token（checkpoint 下发给设备） |
| `xiaozhi_ws.mcp_enabled` | 设备 MCP 工具桥（默认开）——LLM 可调用机器人指令 |
| `xiaozhi_ws.asr.provider` | `siliconflow`（真实）/ `stub`（固定文本联调） |
| `xiaozhi_ws.asr.api_key` | 已直接写入；LLM key 在 `llm_config.json` 同样直接写入 |
| `xiaozhi_ws.tts.enabled` | 语音回复（CosyVoice2，默认开）；关则纯文本 |

**LLM 选型注意**（SiliconFlow 实测，2026-09）：
- `Qwen/Qwen3.5-35B-A3B`：~2 s，支持工具调用，**默认推荐**（思考型，回复含 reasoning）。
- `THUDM/GLM-4-32B-0414`：~1 s，但**带 tools 的请求会 500**，不可用于本网关。
- `Qwen/Qwen3.5-122B-A10B`：该 key 上响应 >4 min，不可用。

## 部署到 3516

3516 上需可运行的 Linux（含 libc）+ 与其架构匹配的静态工具链。流程沿用
LiteCrab 的交叉编译体系：

1. 按 3516 实际架构准备 sysroot（参考 `litecrab/others/5091-build/` 的做法，
   libopus 需一并交叉编译为静态库，或用 `--provider stub` 先上文本链路）。
2. `cmake -S . -B build-3516 -DCMAKE_TOOLCHAIN_FILE=<3516-toolchain.cmake>
   -DCMAKE_BUILD_TYPE=Release -DLITECRAB_BUILD_TESTS=OFF -DLITECRAB_ENABLE_STATIC_LINK=ON
   -DLITECRAB_OPUS_PREFIX=<armhf-opus-prefix>`
3. `scp build-3516/litecrab_server` 与 `config/*.json`、`skills/` 到板子。
4. 板上运行（`SSL_CERT_FILE` 指向 CA bundle，见 litecrab README）。

## 设备固件重定向

固件默认连官方云（`CONFIG_OTA_URL=https://api.tenclass.net/xiaozhi/ota/`）。
让设备连自己的 agent：编译时把 `CONFIG_OTA_URL` 设为 `http://<3516-IP>:8000/`
（sdkconfig 一行，无需改代码），刷一次即可。

**固件编译注意（本机）**：
1. `D:\项目\能源agent\code\xiaozhi-esp32` 路径含中文，**xtensa 工具链/IDF 无法
   在此路径构建**（gcc 读 spec 文件报错，IDF `idf.py` 强制 realpath）。
   已验证方案：镜像到 ASCII 路径构建：
   ```powershell
   robocopy "D:\项目\能源agent\code\xiaozhi-esp32" "D:\xz_build\xiaozhi-esp32" /MIR /XD build build-wsl .git logs .crab third_party __pycache__
   cd D:\xz_build\xiaozhi-esp32
   $env:IDF_TOOLS_PATH="D:\Espressif"; . D:\esp\v6.0.2\esp-idf\export.ps1
   python scripts/build.py otto-robot          # 自动 idf.py build + merge-bin
   ```
   产物 `build\merged-binary.bin`（10.66 MB）已同步回
   `D:\项目\能源agent\code\xiaozhi-esp32\build\`。
2. 本机页面文件较小，全并行 ninja 会报“页面文件太小”；`build.py` 失败后续跑
   `ninja -C build -j 2` 即可，merge-bin 产物不受影响。
3. 脚本测试 `python -m unittest discover -s scripts/tests`：62/67 通过，
   5 个失败为上游测试的 Windows 平台性问题（`os.chdir` 后临时目录无法删除，
   CI 仅 Linux 运行），与本仓库改动无关。

## 烧录

```powershell
cd D:\项目\能源agent\code\xiaozhi-esp32\build
python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset ^
  write-flash 0x0 merged-binary.bin
```

## 安全注意

按当前项目策略，SiliconFlow API key 直接写入 `config/base_config.json` 与
`config/llm_config.json` 并随仓库提交（用户明确要求）。该仓库如需公开或共享，
请先撤销该 key 并改回 `apiKeyEnv` 环境变量方式（配置加载逻辑已支持）。
