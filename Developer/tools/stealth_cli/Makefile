# stealth_cli —— stealth_hook 模块的命令行客户端（用户态，非 KPM）
#
# 与 modules/KPM/*/Makefile 的差别：这里产出的是可执行文件，不是 .kpm，
# 所以不做 -r 链接、不加 -DMYKPM_VERSION。
#
# 用法：
#   export ANDROID_NDK_LATEST_HOME=/path/to/ndk
#   make                 # 产出 stealth_cli（aarch64 静态可执行）
#   make clean

CC_TARGET := aarch64-linux-android31-clang
CFLAGS := -O2 -Wall -Wextra -static
TARGET := stealth_cli
SRC := stealth_cli.c

NDK_ROOT := $(if $(ANDROID_NDK_LATEST_HOME),$(ANDROID_NDK_LATEST_HOME),$(ANDROID_NDK))

ifeq ($(strip $(NDK_ROOT)),)
$(error 需要设置 ANDROID_NDK_LATEST_HOME 或 ANDROID_NDK)
endif

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_S),Linux)
HOST_TAG := linux-$(UNAME_M)
else ifeq ($(UNAME_S),Darwin)
HOST_TAG := darwin-$(UNAME_M)
else
HOST_TAG := windows-x86_64
endif

TOOLCHAIN := $(NDK_ROOT)/toolchains/llvm/prebuilt/$(HOST_TAG)
CC := $(TOOLCHAIN)/bin/$(CC_TARGET)

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
