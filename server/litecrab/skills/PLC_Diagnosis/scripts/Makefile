# PLC 智能诊断程序构建脚本（单二进制多子命令：login/get_cache_info/get_monitor_info/get_history_alarm/set_freq_band）
#
# =============================================================================
# 两种编译方式（按目标运行环境选择其一）
# =============================================================================
#
# 方式一：WSL 本地编译（x86_64，动态链接）
#   适用：WSL 开发和测试
#   前置：sudo apt-get install -y libcurl4-openssl-dev
#   命令：make
#   产物：x86_64 动态链接二进制，不能在 5091 上运行
#
# 方式二：5091 交叉编译（aarch32，全静态链接）
#   适用：部署到 SD5091 设备
#   前置：1) sudo apt-get install -y gcc-arm-linux-gnueabihf
#         2) bash ../../../scripts/build_sd5091_sysroot.sh
#   命令：make CROSS_COMPILE=arm-linux-gnueabihf- STATIC=1 SYSROOT=/opt/sd5091-sysroot
#   产物：ARM 32-bit 全静态二进制，可在 5091 上直接运行
#
# =============================================================================
# 验证
# =============================================================================
#   make check                                      # WSL 本地
#   make check CROSS_COMPILE=arm-linux-gnueabihf-   # 5091 交叉
#   file main                        # 查看架构
#   arm-linux-gnueabihf-readelf -h main | grep Machine   # 应显示 ARM
#
# =============================================================================
# 其他
# =============================================================================
#   make print-config               # 打印当前配置
#   make strip                      # 去符号表（减小体积）
#   make clean                      # 清理

TARGET := main
SRC := main.c $(wildcard asp/*.c) ../../../src/util/json.c
HEADERS := asp/asp.h ../../../include/litecrab/json.h

# 交叉编译器前缀（必须包含最后一个短横线）：
#   5091 交叉编译用 arm-linux-gnueabihf-
#   WSL 本地编译留空
CROSS_COMPILE ?=

CC      := $(CROSS_COMPILE)gcc
STRIP   := $(CROSS_COMPILE)strip
READELF := $(CROSS_COMPILE)readelf

CFLAGS   := -std=gnu11 -O2 -Wall -Wextra
CPPFLAGS := -I../../../include
LDFLAGS  :=

# SYSROOT：5091 交叉编译时指向 armhf sysroot，由 scripts/build_sd5091_sysroot.sh 生成。
# 布局：
#   $(SYSROOT)/usr/include/curl/curl.h
#   $(SYSROOT)/usr/include/openssl/ssl.h
#   $(SYSROOT)/usr/include/zlib.h
#   $(SYSROOT)/usr/lib/libcurl.a libssl.a libcrypto.a libz.a
# WSL 本地编译时留空（用系统头文件和动态库）。
SYSROOT ?=

# 静态链接开关：
#   0 = 动态链接（WSL 本地测试用，需 libcurl4-openssl-dev）
#   1 = 全静态链接（5091 部署用，设备无 libcurl）
STATIC ?= 0

ifneq ($(strip $(SYSROOT)),)
    CPPFLAGS += -I$(SYSROOT)/usr/include
    LDFLAGS  += -L$(SYSROOT)/usr/lib
endif

# libcurl 全静态依赖集合（链接顺序敏感）：
# libcurl.a 依赖 openssl/zlib，openssl 又依赖 dl/pthread。
# sysroot 构建时已关闭 brotli/zstd/nghttp2，无需链接。
CURL_STATIC_LIBS := -lcurl -lssl -lcrypto -lz -ldl -lpthread

# CA_FILE 是程序在 SD5091 上运行时访问的路径，不是编译机上的路径。
ifneq ($(strip $(CA_FILE)),)
    CPPFLAGS += -DDEVICE_CA_FILE='"$(CA_FILE)"'
endif

ifeq ($(STATIC),1)
    # 全静态：把 libcurl 及全部依赖打入 main
    LDFLAGS += -static
    LDLIBS  := $(CURL_STATIC_LIBS)
else
    # 动态链接：仅本机测试用
    LDLIBS  := -lcurl
endif

.PHONY: all clean strip check test-normalized print-config

all: $(TARGET)

$(TARGET): $(SRC) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

strip: $(TARGET)
	$(STRIP) $(TARGET)

check: $(TARGET)
	@echo "=== 二进制架构验证 ==="
	file $(TARGET)
	$(READELF) -h $(TARGET) | grep -E "Class|Machine|OSABI"
	@echo "=== 动态依赖检查 ==="
	$(READELF) -d $(TARGET) 2>/dev/null | grep -q NEEDED \
	    && echo "注意：该二进制仍依赖动态库，5091 上若无对应 .so 将无法运行" \
	    || echo "无 NEEDED 动态依赖，为全静态二进制。"

test-normalized:
	$(MAKE) TARGET=/tmp/litecrab-plc-normalized-test CROSS_COMPILE= STATIC=0 SYSROOT=
	python3 tests/test_normalized_output.py /tmp/litecrab-plc-normalized-test

print-config:
	@echo "CC=$(CC)"
	@echo "STATIC=$(STATIC)"
	@echo "SYSROOT=$(SYSROOT)"
	@echo "CPPFLAGS=$(CPPFLAGS)"
	@echo "CFLAGS=$(CFLAGS)"
	@echo "LDFLAGS=$(LDFLAGS)"
	@echo "LDLIBS=$(LDLIBS)"

clean:
	rm -f $(TARGET)
