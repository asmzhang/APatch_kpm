# wxshadow_client —— wxshadow 模块的用户态客户端（非 KPM）
#
# 上游 `kpms/wxshadow/wxshadow_client.c` 原样搬入，只把构建改成与本仓一致：
# 不依赖 cmake、不依赖 aarch64-linux-gnu-gcc，用 NDK clang 出静态可执行。
#
# 用法：
#   export ANDROID_NDK_LATEST_HOME=/path/to/ndk
#   make                 # 产出 wxshadow_client（aarch64 静态可执行）
#   make clean

CC_TARGET := aarch64-linux-android31-clang
CFLAGS := -O2 -Wall -static
TARGET := wxshadow_client
SRC := wxshadow_client.c

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

CC := $(NDK_ROOT)/toolchains/llvm/prebuilt/$(HOST_TAG)/bin/$(CC_TARGET)

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
