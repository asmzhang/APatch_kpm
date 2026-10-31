MYKPM_VERSION := 2.0.0

KP_DIR ?= ../../../KernelPatch

ifdef ANDROID_NDK_LATEST_HOME
NDK_HOME := $(ANDROID_NDK_LATEST_HOME)
else ifdef ANDROID_NDK
NDK_HOME := $(ANDROID_NDK)
else
$(error 未找到 NDK：请设置 ANDROID_NDK_LATEST_HOME 或 ANDROID_NDK)
endif

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifneq (,$(or $(findstring MINGW,$(UNAME_S)),$(findstring MSYS,$(UNAME_S))))
HOST_TAG := windows-x86_64
else
HOST_TAG := $(shell echo $(UNAME_S) | tr A-Z a-z)-$(UNAME_M)
endif

TOOLCHAIN := $(NDK_HOME)/toolchains/llvm/prebuilt/$(HOST_TAG)/bin
CC := $(TOOLCHAIN)/aarch64-linux-android31-clang
LD := $(TOOLCHAIN)/ld.lld

INCLUDE_DIRS := . include patch/include linux/include linux/arch/arm64/include linux/tools/arch/arm64/include

CFLAGS = -Wall -O2 -fno-PIC -fno-asynchronous-unwind-tables -fno-stack-protector -fno-common \
	-DMYKPM_VERSION=\"$(MYKPM_VERSION)$(PH_VER)\" \
	$(foreach dir,$(INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(dir)) -Isrc

C_FILES := src/main.c src/debugger_hide.c src/frida_hide.c src/openat_hide.c src/net_hide.c src/mem_hide.c
objs := $(C_FILES:.c=.o)
debug_objs := $(addprefix debug/,$(objs))

all: panda_hide_$(MYKPM_VERSION).kpm

debug: CFLAGS += -DCONFIG_DEBUG
debug: PH_VER := _d
debug: panda_hide_$(MYKPM_VERSION)_debug.kpm

panda_hide_$(MYKPM_VERSION).kpm: $(objs)
	$(CC) -r -o $@ $^

panda_hide_$(MYKPM_VERSION)_debug.kpm: $(debug_objs)
	$(CC) -r -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

debug/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.kpm
	rm -rf debug
	find . -name "*.o" -delete

.PHONY: all debug clean
