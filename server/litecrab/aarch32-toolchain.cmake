# aarch32-toolchain.cmake
#
# SD5091 交叉编译工具链文件（aarch32 / armhf）。
#
# SD5091 内核为 aarch64，但用户空间是 32 位 ARM (armv7 hard-float)，
# 动态加载器为 /lib/ld-linux.so.3，因此必须使用 arm-linux-gnueabihf- 交叉编译器。
#
# 用法（在 WSL 上执行）：
#   cmake -S . -B build-5091 \
#     -DCMAKE_TOOLCHAIN_FILE=aarch32-toolchain.cmake \
#     -DLITECRAB_ENABLE_STATIC_LINK=ON
#
# 前置条件：
#   1. 安装交叉编译器：sudo apt-get install -y gcc-arm-linux-gnueabihf
#   2. 构建 sysroot（含 armhf 静态 OpenSSL + zlib + libcurl）：
#      bash scripts/build_sd5091_sysroot.sh
#
# WSL 本地编译 (x86_64) 不需要此文件，直接：
#   cmake -S . -B build && cmake --build build -j

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CROSS_PREFIX "arm-linux-gnueabihf-")
set(CMAKE_C_COMPILER ${CROSS_PREFIX}gcc)

set(SD5091_SYSROOT "/opt/sd5091-sysroot" CACHE PATH
  "SD5091 aarch32 sysroot (built by scripts/build_sd5091_sysroot.sh)")
set(CMAKE_FIND_ROOT_PATH ${SD5091_SYSROOT})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
