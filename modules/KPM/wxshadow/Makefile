MYKPM_VERSION := 1.2.2

ifndef KP_DIR
    KP_DIR = ../../../KernelPatch
endif

OS_NAME = $(shell uname | tr A-Z a-z)
MACHINE = $(shell uname -m)
HOST_TAG = $(OS_NAME)-$(MACHINE)
ifeq ($(HOST_TAG), darwin-arm64)
    HOST_TAG = darwin-x86_64
endif
ifeq ($(patsubst mingw64_nt-10.0-%-x86_64,windows-x86_64,$(HOST_TAG)),windows-x86_64)
    HOST_TAG = windows-x86_64
endif
NDK_BIN_DIR := toolchains/llvm/prebuilt/$(HOST_TAG)/bin
ifdef ANDROID_NDK_LATEST_HOME
    NDK_PATH ?= $(ANDROID_NDK_LATEST_HOME)/$(NDK_BIN_DIR)
else ifdef ANDROID_NDK
    NDK_PATH ?= $(ANDROID_NDK)/$(NDK_BIN_DIR)
endif

ifdef TARGET_COMPILE
    CC := $(TARGET_COMPILE)gcc
else ifdef NDK_PATH
    CC := $(NDK_PATH)/aarch64-linux-android31-clang
endif

# 1.2.1 起源码不再写死上游串：wxshadow.c 用 KPM_VERSION(MYKPM_VERSION)，
# 版本号由下面的 -DMYKPM_VERSION 传进去（改版本只需改这一行）。
CFLAGS = -Wall -O2 -fno-PIC -fno-asynchronous-unwind-tables -fno-stack-protector -fno-common -DMYKPM_VERSION=\"$(MYKPM_VERSION)\"

INCLUDE_DIRS := . include patch/include linux/include linux/arch/arm64/include linux/tools/arch/arm64/include
INCLUDE_FLAGS := $(foreach dir,$(INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(dir))
INCLUDE_FLAGS += -I.

C_FILES := wxshadow.c
C_FILES += wxshadow_pgtable.c
C_FILES += wxshadow_handlers.c
C_FILES += wxshadow_bp.c
C_FILES += wxshadow_scan.c

objs := $(C_FILES:.c=.o)

# 不生成 debug 变体：上游源码里没有任何 debug-only 分支（无 #ifdef 调试宏），
# 加 -D 编出来的字节与 release 完全相同，多一个同哈希产物只会误导身份记录。
all: wxshadow_$(MYKPM_VERSION).kpm

wxshadow_$(MYKPM_VERSION).kpm: ${objs}
	${CC} -r -o $@ $^

%.o: %.c
	${CC} $(CFLAGS) $(INCLUDE_FLAGS) -c -o $@ $<

.PHONY: clean
clean:
	rm -f *.kpm
	find . -name "*.o" | xargs rm -f
