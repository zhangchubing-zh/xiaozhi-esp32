# SD5091 部署指南

> 本文档基于 5091 设备实际探测结果编写，涵盖从编译到运行的全流程。
>
> 当前 Step 0～4 已实现功能、真实边界、正式单二进制部署方式和剩余问题，请先阅读
> [`sd5091_current_implementation_guide.md`](sd5091_current_implementation_guide.md)。如果本文旧步骤与该总览的正式产物边界冲突，以当前实现总览和代码为准。

## 1. 设备环境摘要

| 项目 | 实际值 |
|---|---|
| 内核 | Linux 5.10.0 aarch64 SMP PREEMPT |
| 用户空间架构 | **32 位 ARM (armv7 hard-float)**，ELF32 / EM_ARM |
| 动态加载器 | `/lib/ld-linux.so.3` |
| OS | RTOS 208.8.1.SPC0700 (busybox 1.34.1) |
| C 运行库 | glibc (libc.so.6 + 独立 libpthread.so.0) |
| 编译工具 | **无** (gcc/cmake/make/ar 全部缺失) |
| Python3 | **无** |
| libcurl | **无** |
| OpenSSL | 3.0.9 (动态库，`/lib/libssl.so.3` → `/app_run/app_bin/lib/libssl.so.3.0.9`) |
| zlib | 1.3.1 (`/lib/libz.so.1`) |
| CA 证书 | **无** |
| DNS | **未配置** (resolv.conf 为空) |
| 可写目录 | 仅 `/mnt/home/enspire` (HOME) |
| 内存 | 724M total, ~320M available |
| ulimit -n | 1024 |
| init | busybox init (无 systemd) |

### 关键结论

1. 5091 上**不能编译**，必须从 WSL 交叉编译。
2. 用户空间是 **32 位 ARM**，不是 aarch64。必须使用 `arm-linux-gnueabihf-gcc`。
3. 设备有 OpenSSL/zlib 动态库但无 libcurl，skill 程序必须全静态链接。
4. DNS 和 CA 证书缺失，需在部署时手动修复。

---

## 2. WSL 编译环境准备

```sh
# 安装交叉编译器和构建工具
sudo apt-get update
sudo apt-get install -y \
  gcc-arm-linux-gnueabihf \
  build-essential cmake make perl \
  curl wget
```

验证交叉编译器：

```sh
arm-linux-gnueabihf-gcc --version
# 应输出版本信息
arm-linux-gnueabihf-gcc -dumpmachine
# 应输出 arm-linux-gnueabihf
```

---

## 3. 构建 armhf sysroot（一次即可）

sysroot 包含 5091 所需的全静态库：zlib + OpenSSL 3.0.9 + libcurl。

```sh
cd /path/to/LiteCrab_0829

bash scripts/build_sd5091_sysroot.sh
# 默认安装到 /opt/sd5091-sysroot
# 自定义路径：SYSROOT=/my/path bash scripts/build_sd5091_sysroot.sh
```

构建过程下载源码并编译，耗时约 10-20 分钟（取决于网络和 CPU）。

验证 sysroot：

```sh
ls /opt/sd5091-sysroot/usr/lib/lib*.a
# 应有: libz.a libssl.a libcrypto.a libcurl.a

ls /opt/sd5091-sysroot/usr/include/openssl/ssl.h
ls /opt/sd5091-sysroot/usr/include/curl/curl.h
ls /opt/sd5091-sysroot/usr/include/zlib.h
```

---

## 4. 交叉编译主项目

```sh
cmake -S . -B build-5091 \
  -DCMAKE_TOOLCHAIN_FILE=aarch32-toolchain.cmake \
  -DLITECRAB_ENABLE_STATIC_LINK=ON \
  -DSD5091_SYSROOT=/opt/sd5091-sysroot

cmake --build build-5091 -j
```

### 验证二进制

```sh
# 架构检查
file build-5091/litecrab_server
# 期望: ELF 32-bit LSB pie executable, ARM, EABI5, hard-float, ...

# Machine 字段
arm-linux-gnueabihf-readelf -h build-5091/litecrab_server | grep -E 'Class|Machine'
# 期望: Class: ELF32 / Machine: ARM

# 静态链接验证
arm-linux-gnueabihf-readelf -d build-5091/litecrab_server | grep NEEDED
# 期望: 无输出（全静态，无动态依赖）
```

> 如果 `NEEDED` 有输出，说明仍有动态依赖，5091 上可能无法运行。检查 sysroot 是否完整。

---

## 5. 交叉编译 PLC_Diagnosis skill

```sh
cd skills/PLC_Diagnosis/scripts

make CROSS_COMPILE=arm-linux-gnueabihf- STATIC=1 SYSROOT=/opt/sd5091-sysroot

# 验证
make check CROSS_COMPILE=arm-linux-gnueabihf-
# 期望: ELF 32-bit LSB ... ARM, 无 NEEDED 动态依赖
```

---

## 6. 部署到 5091

### 6.1 传输文件

```sh
# 替换 <5091-ip> 为实际地址
TARGET="enspire@<5091-ip>:/mnt/home/enspire/litecrab"

ssh $TARGET "mkdir -p /mnt/home/enspire/litecrab"

# 主程序
scp build-5091/litecrab_server $TARGET/

# 配置
scp -r config/ $TARGET/

# Skills（含编译好的 main）
scp -r skills/ $TARGET/

# 头文件（workspace_summary 等 skill 可能需要）
scp -r include/ $TARGET/

# CA 证书（从 WSL 复制）
scp /etc/ssl/certs/ca-certificates.crt $TARGET/ca-bundle.crt
```

### 6.2 配置 5091 环境

SSH 登录 5091 后执行：

```sh
cd /mnt/home/enspire/litecrab

# 1. 配置 DNS（需 root）
su -c 'echo "nameserver 8.8.8.8" > /etc/resolv.conf'
# 或使用设备内网 DNS 服务器地址

# 2. 验证 DNS
nslookup ai-api.jiapi.com
# 应能解析出 IP

# 3. 复制配置
cp config/base_config.example.json config/base_config.json
cp config/llm_config.example.json config/llm_config.json

# 4. 编辑配置（按需修改 LLM Provider、模型等）
vi config/llm_config.json

# 5. 设置 API Key
export RELAYHUB_API_KEY=your-key

# 6. 设置 CA 证书路径（OpenSSL 3.x 读取此环境变量）
export SSL_CERT_FILE=/mnt/home/enspire/litecrab/ca-bundle.crt

# 7. 确保 main 有执行权限
chmod +x litecrab_server
chmod +x skills/PLC_Diagnosis/scripts/main
```

### 6.3 启动服务

```sh
cd /mnt/home/enspire/litecrab

./litecrab_server \
  --config config/base_config.json \
  --llm-config config/llm_config.json \
  --llm-provider relayhub
```

### 6.4 测试 TCP 连接

在 5091 上用 busybox 测试：

```sh
# 发送 JSON 请求
echo '{"userId":"test","content":"你好"}' | busybox nc 127.0.0.1 10003

# 或纯文本
echo "读取 README.md" | busybox nc 127.0.0.1 10003
```

---

## 7. 运行环境限制与解决方案

### 7.1 DNS 未配置

**问题**：`/etc/resolv.conf` 为空（1 字节），`nslookup` 超时，LLM Provider 域名无法解析。

**解决**（需 root）：
```sh
su -c 'echo "nameserver 8.8.8.8" > /etc/resolv.conf'
# 或使用设备内网 DNS
su -c 'echo "nameserver 192.168.0.1" > /etc/resolv.conf'
```

> 注意：设备重启后 resolv.conf 可能被重置，需重新配置或加入启动脚本。

### 7.2 无 CA 证书

**问题**：HTTPS 证书校验失败，无法连接 LLM Provider。

**解决**：从 WSL 复制 CA 证书并设置环境变量：
```sh
# 在 WSL 上
scp /etc/ssl/certs/ca-certificates.crt enspire@<5091-ip>:/mnt/home/enspire/litecrab/ca-bundle.crt

# 在 5091 上
export SSL_CERT_FILE=/mnt/home/enspire/litecrab/ca-bundle.crt
```

> 主项目代码使用 `SSL_CTX_set_default_verify_paths()` 加载系统默认 CA 路径。OpenSSL 3.x 也读取 `SSL_CERT_FILE` 环境变量。如果环境变量不生效，可将 ca-bundle.crt 放到 OpenSSL 默认搜索路径（需 root）：
> ```sh
> su -c 'mkdir -p /etc/ssl/certs && cp /mnt/home/enspire/litecrab/ca-bundle.crt /etc/ssl/certs/ca-certificates.crt'
> ```

### 7.3 无 Python3

**问题**：Transfer Station 和部分 E2E 测试无法在 5091 上运行。

**解决**：Transfer Station 在 PC 上运行，代理请求到 5091。详见 README 中 "PC Transfer Station" 部分。

### 7.4 可写目录限制

**问题**：只有 `/mnt/home/enspire` (HOME) 可写。日志、会话快照必须放在 HOME 下。

**解决**：`base_config.json` 中配置相对路径（相对于 workspace_root），workspace_root 设为 `.`（即 HOME 下的 litecrab 目录）：
```json
{
  "log": { "dir": "logs", "level": "INFO" },
  "workspace_root": "."
}
```

### 7.5 文件描述符上限

**问题**：`ulimit -n` 为 1024，高并发时可能不够。

**解决**（需 root）：
```sh
su -c 'ulimit -n 65536 && /mnt/home/enspire/litecrab/litecrab_server ...'
```

### 7.6 内存限制

**问题**：总内存 724M，可用约 320M。LLM 请求（特别是流式响应）可能占用较多内存。

**解决**：监控内存使用，必要时调整 LLM 配置中的 `max_tokens` 和 `timeout_ms`。5091 上的 `max_tokens` 建议不超过 2048。

### 7.7 无 systemd

**问题**：设备使用 busybox init，没有 systemd 服务管理。

**解决**：手动启动或编写启动脚本放入 `/etc/init.d/`（需 root）：
```sh
# /etc/init.d/S99litecrab (示例)
#!/bin/sh
case "$1" in
  start)
    echo "Starting litecrab..."
    cd /mnt/home/enspire/litecrab
    export RELAYHUB_API_KEY=your-key
    export SSL_CERT_FILE=/mnt/home/enspire/litecrab/ca-bundle.crt
    start-stop-daemon -S -b -m -p /var/run/litecrab.pid \
      -x /mnt/home/enspire/litecrab/litecrab_server -- \
      --config config/base_config.json \
      --llm-config config/llm_config.json \
      --llm-provider relayhub
    ;;
  stop)
    start-stop-daemon -K -p /var/run/litecrab.pid
    ;;
  *)
    echo "Usage: $0 {start|stop}"
    exit 1
esac
```

> 注：`/var/run` 可能不可写，pid 文件路径需调整到 HOME 下。

---

## 8. PLC_Diagnosis skill 在 5091 上的运行

skill 的 `main` 二进制已全静态链接，可直接在 5091 上运行。它连接 PLC 设备 `https://192.168.8.10`，不需要 DNS 和外部 CA 证书（测试期 TLS 校验已关闭）。

```sh
cd /mnt/home/enspire/litecrab

# 测试登录
./skills/PLC_Diagnosis/scripts/main -o login

# 读监控信号（含频段 8201）
./skills/PLC_Diagnosis/scripts/main -o get_monitor_info

# 改频段
./skills/PLC_Diagnosis/scripts/main -o set_freq_band --value 23
```

> 192.168.8.10 在设备路由表中（`192.168.8.0/24 dev eth0.200`），网络可达。

---

## 9. 验证清单

部署完成后逐项验证：

- [ ] `file litecrab_server` 显示 ELF 32-bit ARM
- [ ] `file skills/PLC_Diagnosis/scripts/main` 显示 ELF 32-bit ARM
- [ ] `nslookup <LLM-provider-domain>` 能解析
- [ ] `./litecrab_server --config config/base_config.json --llm-config config/llm_config.json` 能启动
- [ ] `echo '你好' | busybox nc 127.0.0.1 10003` 能收到回复
- [ ] `./skills/PLC_Diagnosis/scripts/main -o login` 能登录 PLC 设备
- [ ] LLM 请求成功返回（检查日志 `logs/` 目录）

---

## 10. 故障排查

### 二进制无法执行

```
./litecrab_server: No such file or directory
```

原因：架构不匹配。验证 `file litecrab_server` 输出是否为 ARM 32-bit。如果是 x86_64，说明编译时未使用工具链文件。

### TLS 握手失败

```
SSL_connect failed / certificate verify failed
```

原因：无 CA 证书。设置 `SSL_CERT_FILE` 环境变量，或将 ca-bundle.crt 放到 `/etc/ssl/certs/`。

### DNS 解析失败

```
getaddrinfo failed (-201)
```

原因：resolv.conf 为空。配置 DNS 服务器。

### 连接 LLM 超时

```
timeout (-201)
```

原因：网络不通或防火墙。检查路由和连通性：
```sh
busybox ping <LLM-provider-IP>
busybox telnet <LLM-provider-domain> 443
```

### Hub 队列满

```
busy / queue full
```

原因：并发请求超过 Hub 容量（默认 16）。等待重试或减少并发。
