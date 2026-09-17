#!/bin/bash
# =============================================================================
# build_sd5091_sysroot.sh — 构建 SD5091 (armhf) 全静态 sysroot
#
# 构建：zlib + OpenSSL + libcurl，全部静态链接 (.a)，目标 arm-linux-gnueabihf
# 安装到 $SYSROOT，供 aarch32-toolchain.cmake 和 skills/PLC_Diagnosis/scripts/Makefile 使用
#
# 用法：
#   bash scripts/build_sd5091_sysroot.sh                     # 默认 SYSROOT=/opt/sd5091-sysroot
#   SYSROOT=/my/path bash scripts/build_sd5091_sysroot.sh    # 自定义路径
#   CURL_VERSION=8.8.0 bash scripts/build_sd5091_sysroot.sh  # 自定义 curl 版本
#
# 前置条件（WSL 上安装一次即可）：
#   sudo apt-get install -y gcc-arm-linux-gnueabihf make perl
#   # 下载工具（二选一）：
#   sudo apt-get install -y wget   # 或 curl
#
# 完成后用 CMake 交叉编译主项目：
#   cmake -S . -B build-5091 \
#     -DCMAKE_TOOLCHAIN_FILE=aarch32-toolchain.cmake \
#     -DLITECRAB_ENABLE_STATIC_LINK=ON \
#     -DSD5091_SYSROOT=$SYSROOT
#   cmake --build build-5091 -j
#
# 用 Makefile 编译 PLC_Diagnosis skill：
#   cd skills/PLC_Diagnosis/scripts
#   make CROSS_COMPILE=arm-linux-gnueabihf- STATIC=1 SYSROOT=$SYSROOT
# =============================================================================
set -euo pipefail

# ---- 配置 -----------------------------------------------------------------
CROSS="${CROSS:-arm-linux-gnueabihf-}"
SYSROOT="${SYSROOT:-/opt/sd5091-sysroot}"
ZLIB_VERSION="${ZLIB_VERSION:-1.3.1}"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.0.9}"   # 与 5091 设备上的版本一致
CURL_VERSION="${CURL_VERSION:-8.11.0}"
BUILD_DIR="${BUILD_DIR:-/tmp/sd5091-build}"
NPROC="$(nproc 2>/dev/null || echo 4)"

CC="${CROSS}gcc"
AR="${CROSS}ar"
RANLIB="${CROSS}ranlib"
STRIP="${CROSS}strip"

# ---- 颜色输出 -------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'
log()  { echo -e "${GREEN}[$(date +%H:%M:%S)]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
err()  { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# ---- 前置检查 -------------------------------------------------------------
command -v "$CC" >/dev/null 2>&1 || { err "$CC 未找到。安装：sudo apt-get install -y gcc-arm-linux-gnueabihf"; exit 1; }
command -v make >/dev/null 2>&1    || { err "make 未找到。安装：sudo apt-get install -y make"; exit 1; }
command -v perl >/dev/null 2>&1    || { err "perl 未找到 (OpenSSL 构建需要)。安装：sudo apt-get install -y perl"; exit 1; }

download() {
    local url="$1" dest="$2"
    if command -v wget >/dev/null 2>&1; then
        wget -q -O "$dest" "$url"
    elif command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$dest" "$url"
    else
        err "需要 wget 或 curl 来下载源码"; exit 1
    fi
}

# ---- 开始 -----------------------------------------------------------------
log "=== SD5091 armhf sysroot 构建 ==="
log "交叉编译器: $CC"
log "SYSROOT: $SYSROOT"
log "版本: zlib=$ZLIB_VERSION openssl=$OPENSSL_VERSION curl=$CURL_VERSION"
log "构建目录: $BUILD_DIR"

mkdir -p "$BUILD_DIR" "$SYSROOT"
cd "$BUILD_DIR"

# ============================================================================
# 1. zlib
# ============================================================================
ZLIB_TARBALL="zlib-$ZLIB_VERSION.tar.gz"
ZLIB_URL="https://zlib.net/fossils/$ZLIB_TARBALL"
if [ ! -f "$SYSROOT/usr/lib/libz.a" ]; then
    log "构建 zlib $ZLIB_VERSION ..."
    [ -f "$ZLIB_TARBALL" ] || download "$ZLIB_URL" "$ZLIB_TARBALL"
    tar xf "$ZLIB_TARBALL"
    cd "zlib-$ZLIB_VERSION"
    CHOST="${CROSS%-}" ./configure --static --prefix="$SYSROOT/usr"
    make -j"$NPROC"
    make install
    cd ..
    log "zlib 完成"
else
    log "zlib 已存在，跳过"
fi

# ============================================================================
# 2. OpenSSL
# ============================================================================
OPENSSL_TARBALL="openssl-$OPENSSL_VERSION.tar.gz"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/$OPENSSL_TARBALL"
if [ ! -f "$SYSROOT/usr/lib/libssl.a" ]; then
    log "构建 OpenSSL $OPENSSL_VERSION ..."
    [ -f "$OPENSSL_TARBALL" ] || download "$OPENSSL_URL" "$OPENSSL_TARBALL"
    tar xf "$OPENSSL_TARBALL"
    cd "openssl-$OPENSSL_VERSION"

    # OpenSSL 交叉编译：linux-armv4 是通用 arm 目标，配合编译器 flag 适配 armv7
    ./Configure linux-armv4 \
        no-shared no-tests \
        --prefix="$SYSROOT/usr" \
        --cross-compile-prefix="$CROSS" \
        -DOPENSSL_USE_NODELETE

    make -j"$NPROC"
    make install_sw install_ssldirs
    cd ..
    log "OpenSSL 完成"
else
    log "OpenSSL 已存在，跳过"
fi

# ============================================================================
# 2. OpenSSL 和上面的二选一
# ============================================================================
OPENSSL_TARBALL="openssl-$OPENSSL_VERSION.tar.gz"
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/$OPENSSL_TARBALL"
if [ ! -f "$SYSROOT/usr/lib/libssl.a" ]; then
    log "构建 OpenSSL $OPENSSL_VERSION ..."
    [ -f "$OPENSSL_TARBALL" ] || download "$OPENSSL_URL" "$OPENSSL_TARBALL"
    tar xf "$OPENSSL_TARBALL"
    cd "openssl-$OPENSSL_VERSION"

    # OpenSSL 交叉编译：直接使用外部传入的 CC/AR/RANLIB/NM/LD
    # 不再使用 --cross-compile-prefix，避免与外部变量双重拼接
    ./Configure linux-armv4 \
        no-shared no-tests \
        --prefix="$SYSROOT/usr" \
        -DOPENSSL_USE_NODELETE

    make -j"$NPROC"
    make install_sw install_ssldirs
    cd ..
    log "OpenSSL 完成"
else
    log "OpenSSL 已存在，跳过"
fi

# ============================================================================
# 3. libcurl
# ============================================================================
CURL_TARBALL="curl-$CURL_VERSION.tar.gz"
CURL_URL="https://curl.se/download/$CURL_TARBALL"
if [ ! -f "$SYSROOT/usr/lib/libcurl.a" ]; then
    log "构建 libcurl $CURL_VERSION ..."
    [ -f "$CURL_TARBALL" ] || download "$CURL_URL" "$CURL_TARBALL"
    tar xf "$CURL_TARBALL"
    cd "curl-$CURL_VERSION"

    # curl 交叉编译配置缓存
    cat > cross_config.cache <<'EOF'
ac_cv_file__dev_random=yes
ac_cv_func_gethostbyname_r=yes
curl_cv_func_gethostbyname_r_arg=6
EOF

    # 关闭所有可选依赖，只保留 OpenSSL + zlib，确保静态链接无遗漏
    ./configure \
        --host="arm-linux-gnueabihf" \
        --prefix="$SYSROOT/usr" \
        --cache-file=cross_config.cache \
        --with-openssl="$SYSROOT/usr" \
        --with-zlib="$SYSROOT/usr" \
        --without-brotli \
        --without-zstd \
        --without-nghttp2 \
        --without-ngtcp2 \
        --without-nghttp3 \
        --without-libpsl \
        --without-libssh2 \
        --without-librtmp \
        --without-libidn2 \
        --without-schannel \
        --without-secure-transport \
        --without-amissl \
        --disable-shared \
        --enable-static \
        --disable-ldap \
        --disable-ldaps \
        --disable-rtsp \
        --disable-dict \
        --disable-telnet \
        --disable-tftp \
        --disable-pop3 \
        --disable-imap \
        --disable-smb \
        --disable-smtp \
        --disable-gopher \
        --disable-mqtt \
        --disable-manual \
        --disable-docs \
        CC="$CC" \
        AR="$AR" \
        RANLIB="$RANLIB" \
        CFLAGS="-I$SYSROOT/usr/include -O2" \
        LDFLAGS="-L$SYSROOT/usr/lib"

    make -j"$NPROC"
    make install
    cd ..
    log "libcurl 完成"
else
    log "libcurl 已存在，跳过"
fi

# ============================================================================
# 验证
# ============================================================================
log "=== 验证 ==="
echo "静态库清单："
ls -l "$SYSROOT/usr/lib/libz.a" \
      "$SYSROOT/usr/lib/libssl.a" \
      "$SYSROOT/usr/lib/libcrypto.a" \
      "$SYSROOT/usr/lib/libcurl.a" 2>/dev/null

echo
echo "头文件检查："
ls "$SYSROOT/usr/include/openssl/ssl.h" \
   "$SYSROOT/usr/include/curl/curl.h" \
   "$SYSROOT/usr/include/zlib.h" 2>/dev/null

echo
ARCH="$("$CC" -dumpmachine 2>/dev/null || echo 'arm-linux-gnueabihf')"
log "交叉编译器 machine: $ARCH"
log "=== sysroot 构建完成 ==="
log ""
log "下一步："
log "  1. 主项目编译："
log "     cmake -S . -B build-5091 -DCMAKE_TOOLCHAIN_FILE=aarch32-toolchain.cmake -DLITECRAB_ENABLE_STATIC_LINK=ON -DSD5091_SYSROOT=$SYSROOT"
log "     cmake --build build-5091 -j"
log "  2. PLC_Diagnosis skill 编译："
log "     cd skills/PLC_Diagnosis/scripts && make CROSS_COMPILE=arm-linux-gnueabihf- STATIC=1 SYSROOT=$SYSROOT"
log "  3. 验证二进制架构："
log "     file build-5091/litecrab_server"
log "     arm-linux-gnueabihf-readelf -h build-5091/litecrab_server | grep Machine"
