# LiteCrab

LiteCrab 是按照 `docs/` 设计文档重新实现的 C11 Agent Runtime，并同步了历史版本中仍有价值的功能设计（未复制历史源码）：

- 有界、线程安全的消息总线与持久 TCP 会话；
- 类型化文件、目录浏览（`ls`/`pwd`）、搜索、CSV、Skill，以及不经过 shell 的 `exec_program` 工具；
- OpenAI 兼容的 HTTP/HTTPS、SSE、chunked 解码和工具调用；
- 会话裁剪、Skill 加载/挂起/恢复，以及可跨轮保存的 Working Memory；
- JSONL trace、JSON/纯文本 TCP 请求兼容；
- JSON 配置、多 Provider 选择、环境变量及命令行覆盖；
- 可选的 ASP 活动告警轮询/订阅监控；
- 独立 PC Transfer Station（TCP 板端代理、LLM HTTP 反向代理、健康检查）。

## 配置准备

以下命令都从 LiteCrab 仓库根目录执行。先复制 LLM 配置并设置对应 Provider 的 API Key：

```sh
cp config/llm_config.example.json config/llm_config.json
export SILICONFLOW_API_KEY=your-key
```

`--llm-provider` 可覆盖配置中的 `llm.default`。API Key 也可直接写成 `apiKey`，但不建议将明文密钥提交到仓库。配置优先级为：默认值 < JSON 配置 < `LITECRAB_*` 环境变量 < 命令行。旧的两个位置参数 `/workspace 10003` 仍兼容。

## WSL 本机构建与运行（x86_64）

原生 Windows 构建会明确拒绝；请在 WSL 或其他 POSIX 环境执行。先安装一次依赖：

```sh
sudo apt update
sudo apt install -y build-essential cmake libssl-dev libcurl4-openssl-dev
```

### 1. 编译和验证

告警能力始终编入同一个二进制，不再使用 `LITECRAB_ENABLE_ALARM` 区分构建。是否启动告警监控只由运行参数决定。

```sh
cmake -S . -B build-wsl \
  -DCMAKE_BUILD_TYPE=Release \
  -DLITECRAB_ENABLE_STATIC_LINK=OFF
cmake --build build-wsl -j
ctest --test-dir build-wsl -R alarm --output-on-failure
```

### 2. 不打开告警运行

不传告警参数时，不启动告警线程：

```sh
./build-wsl/litecrab_server \
  --config config/base_config.example.json \
  --llm-config config/llm_config.json \
  --llm-provider siliconflow \
  --workspace .
```

### 3. 打开告警运行

运行时提供设备告警密码并传入 `--alarm`。默认是轮询模式：启动后立即查询一次，之后每 5 秒查询活动告警。

```sh
export LITECRAB_ALARM_USER=your-device-user
export LITECRAB_ALARM_PASSWORD=your-device-password
./build-wsl/litecrab_server \
  --config config/base_config.example.json \
  --llm-config config/llm_config.json \
  --llm-provider siliconflow \
  --workspace . \
  --alarm-base-url https://192.168.8.10 \
  --alarm-insecure-tls \
  --alarm
```

订阅模式在启动命令末尾增加：

```sh
  --alarm-mode subscription
```

可按需通过 `--alarm-base-url`、`--alarm-user`、`--alarm-ca-file` 覆盖设备参数。认证凭据默认从环境变量 `LITECRAB_ALARM_USER`/`LITECRAB_ALARM_PASSWORD` 读取，也可用 `--alarm-user`/`--alarm-password` 覆盖；不应在命令行中传入明文密码。开发环境临时使用自签名证书时可传 `--alarm-insecure-tls`；正式部署应配置 CA，不应关闭 TLS 校验。连接参数和环境变量只提供配置，不会自行开启监控；必须显式传入 `--alarm` 或 `--alarm-mode`。

### 4. 编译 PLC_Diagnosis scripts

Skill scripts 是独立程序，不区分主程序是否打开告警。WSL 本机编译和检查：

```sh
make -C skills/PLC_Diagnosis/scripts clean
make -C skills/PLC_Diagnosis/scripts
make -C skills/PLC_Diagnosis/scripts check
```

产物为 `skills/PLC_Diagnosis/scripts/main`，是 x86_64 动态链接程序，仅用于 WSL 开发和测试。

## 5091 交叉编译与运行（ARM 32-bit hard-float）

5091 内核为 aarch64，但用户空间是 32 位 ARM（armv7 hard-float），动态加载器为 `/lib/ld-linux.so.3`。设备上没有 gcc、cmake、make、libcurl 或 Python 3，必须在 WSL 交叉编译并全静态链接，然后把产物复制到 5091 运行。普通 x86_64 WSL 没有 `qemu-arm` 时不能直接运行 ARM 二进制。

先准备一次交叉编译环境：

```sh
sudo apt-get install -y gcc-arm-linux-gnueabihf make perl curl
bash others/5091-build/scripts/build_sd5091_sysroot.sh
```

默认 sysroot 为 `/opt/sd5091-sysroot`。更多设备说明见 [5091 部署指南](docs/deployment/sd5091_deployment.md)。

### 1. 交叉编译和验证

告警监控始终编入同一个 ARM 二进制，在设备运行时选择是否开启：

```sh
cmake -S . -B build-5091 \
  -DCMAKE_TOOLCHAIN_FILE=aarch32-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DLITECRAB_BUILD_TESTS=OFF \
  -DLITECRAB_ENABLE_STATIC_LINK=ON
cmake --build build-5091 -j
```

在 WSL 验证产物架构和静态链接：

```sh
file build-5091/litecrab_server
arm-linux-gnueabihf-readelf -h build-5091/litecrab_server | grep Machine
arm-linux-gnueabihf-readelf -d build-5091/litecrab_server |
  grep NEEDED || echo "全静态"
```

部署：

```sh
scp build-5091/litecrab_server \
  enspire@<5091-ip>:/mnt/home/enspire/litecrab/litecrab_server
scp -r config skills \
  enspire@<5091-ip>:/mnt/home/enspire/litecrab/
scp /etc/ssl/certs/ca-certificates.crt \
  enspire@<5091-ip>:/mnt/home/enspire/litecrab/ca-bundle.crt
```

### 2. 不打开告警运行

```sh
cd /mnt/home/enspire/litecrab
su -c 'echo "nameserver 8.8.8.8" > /etc/resolv.conf'
export SSL_CERT_FILE=/mnt/home/enspire/litecrab/ca-bundle.crt
export SILICONFLOW_API_KEY=your-key
./litecrab_server \
  --llm-config config/llm_config.json \
  --llm-provider siliconflow \
  --workspace .
```

### 3. 打开告警运行

```sh
cd /mnt/home/enspire/litecrab
su -c 'echo "nameserver 8.8.8.8" > /etc/resolv.conf'
export SSL_CERT_FILE=/mnt/home/enspire/litecrab/ca-bundle.crt
export SILICONFLOW_API_KEY=your-key
export LITECRAB_ALARM_USER=your-device-user
export LITECRAB_ALARM_PASSWORD=your-device-password
./litecrab_server \
  --llm-config config/llm_config.json \
  --llm-provider siliconflow \
  --workspace . \
  --alarm
```

默认是轮询模式；订阅模式增加 `--alarm-mode subscription`。

### 4. 交叉编译 PLC_Diagnosis scripts

Skill scripts 仍是独立程序，只需编译一次：

```sh
make -C skills/PLC_Diagnosis/scripts clean
make -C skills/PLC_Diagnosis/scripts \
  CROSS_COMPILE=arm-linux-gnueabihf- \
  STATIC=1 \
  SYSROOT=/opt/sd5091-sysroot
make -C skills/PLC_Diagnosis/scripts check \
  CROSS_COMPILE=arm-linux-gnueabihf- \
  STATIC=1 \
  SYSROOT=/opt/sd5091-sysroot
```

验证产物：

```sh
file skills/PLC_Diagnosis/scripts/main
arm-linux-gnueabihf-readelf -h skills/PLC_Diagnosis/scripts/main | grep Machine
arm-linux-gnueabihf-readelf -d skills/PLC_Diagnosis/scripts/main |
  grep NEEDED || echo "全静态"
```

将 ARM 版本的 `main` 随 `skills/PLC_Diagnosis/` 一起部署到 5091。不要在普通 x86_64 WSL 中直接执行它。

## 告警监控行为

当前告警模块的组件边界、持久化状态机、投递流程和恢复语义见
[`docs/alarm/alarm_module_architecture.md`](docs/alarm/alarm_module_architecture.md)。

轮询与订阅代码分别位于 `src/alarm/polling/` 和 `src/alarm/subscription/`，公共登录与 Agent 投递逻辑位于 `src/alarm/`。两种模式最终都查询同一个活动告警接口，并将同样格式的告警推送给 Agent。

每次告警发生使用 `equipid + almid + seqno + localtime` 作为业务身份，并由 canonical 内容生成持久化 key。新告警先写入 `<workspace>/.crab/alarm/alarm_spool.bin` 的固定 delivery slot，再允许 Dispatcher 投递；只有 Agent 完成处理并返回 `HANDLED`，告警线程才通过两阶段持久化确认将 key 记入 seen 索引并释放 slot。这样，进程异常退出后仍可从 spool 恢复未完成告警，不会因仅存在于内存而丢失。

查看每次成功查询、失败重连及当前告警数量：

```sh
tail -F logs/litecrab.log |
  grep --line-buffered -E '\[alarm\]|LiteCrab starting'
```

成功轮询会输出 `active query ok mode=polling alarms=<数量>`。投递组件会记录入队、尝试次数、完成或失败状态；单次告警最多执行首次投递加 3 次重试，并受 180 秒总期限约束。监控向 Agent 发送提取后的字段，而不是完整 ASP JSON：

```text
告警ID ：xxx,
告警名称 ：xxx,
告警原因 ：xxx,
设备ID ：xxx,
告警序列号 ：xxx,
告警时间 ：xxx,
...
使用 PLC_Diagnosis 完整告警诊断、修复和回归验证
```

告警投递是同步等待 Agent 结果的两阶段确认流程。PLC_Diagnosis 负责告警复核、诊断、修复和回归验证，过程通过 Agent 会话 trace 审计。`HANDLED` 只表示这次告警请求已完成处理并生成结论，不等于现场物理告警一定消失；例如参数写入成功但复查告警仍存在时，Skill 应停止继续修改并报告“参数修复成功、问题修复未通过”。

## TCP 请求与会话

每个 TCP 请求以换行结束，既可发送 JSON：

```json
{"userId":"alice","sessionId":"device-5091-line-a","content":"Read README.md"}
```

也可直接发送纯文本。响应同样以换行结束。JSON 请求中的稳定 `sessionId` 可跨 TCP 重连和服务重启恢复；会话快照保存在 `<workspace>/.crab/sessions/`。省略时使用连接级临时 Session。文件工具会阻止绝对路径、目录穿越及符号链接逃逸；HTTPS 校验证书链和主机名。

也可使用仅负责转发原生配置参数的便捷入口：

```sh
python3 tests/run_agent_with_config.py --llm-provider siliconflow
```

## TCP 暴露范围与身份限制

- 默认只监听 `127.0.0.1`，不暴露到其他网络接口。
- Loopback TCP 仍没有客户端认证，同设备的其他进程仍可连接。
- 请求 JSON 中的 `userId` 当前只用于会话兼容显示，不是可信身份，可被任意客户端自报。
- 非 loopback 监听需要在 Base Config 中显式设置 `allow_unauthenticated_remote: true`，这是一个高风险兼容开关，不提供环境变量和命令行覆盖。
- 该开关是明确接受未认证远程 TCP 的风险确认，不代表连接已安全或已认证。生产跨主机访问应通过设备已有认证代理或等待后续认证能力。
- PC Transfer Station 如果需要从另一台机器连接板端 LiteCrab，必须显式配置该兼容模式并承担风险；默认示例配置保持 loopback 和 `false`。

`others/UI/app.py` 会把浏览器会话 UUID 作为 Agent `sessionId`，并为每次请求建立短 TCP 连接。因此重启 `litecrab_server` 不需要重启 UI，也不会复用已经失效的 Windows socket。可通过 `LITECRAB_UI_USER_ID` 配置 UI 使用的稳定用户标识。

## PC Transfer Station

Transfer Station 只依赖 Python 标准库，在 PC 上运行（5091 无 Python 3）：

```sh
cp others/transfer_station/config.example.json others/transfer_station/config.json
export LITECRAB_API_KEY=your-key
cd others/transfer_station
python3 -m transfer_station.main --config config.json
```

它提供：

- TCP 用户请求到 LiteCrab 板端服务的逐行代理；
- `POST /v1/chat/completions` 到真实 LLM 的反向代理与有限重试；
- `GET /health`、`GET /status` 和 CORS/OPTIONS；
- 请求大小限制、JSON 校验、默认模型补全、轮转日志和信号优雅退出。

示例配置中的密钥为空。不要复用或提交历史仓库中曾出现过的明文密钥。

## Skill

Skill 位于仓库的 `skills/<skill-name>/SKILL.md`。内置的 `workspace_summary` 用于验证 Skill 选择、`skill_read` 和后续工具执行链路。

PLC_Diagnosis 的 `scripts/main.c` 使用 libcurl，独立于主程序构建。WSL 和 5091 的编译命令分别列在对应章节。
