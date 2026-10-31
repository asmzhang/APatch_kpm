MYKPM_VERSION := 1.1.2

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
    LD := $(TARGET_COMPILE)ld
else ifdef NDK_PATH
    CC := $(NDK_PATH)/aarch64-linux-android31-clang
    LD := $(NDK_PATH)/ld.lld
endif

CFLAGS = -Wall -O2 -fno-PIC -fno-asynchronous-unwind-tables -fno-stack-protector -fno-common -DMYKPM_VERSION=\"$(MYKPM_VERSION)$(XIIBA_VER)\"

INCLUDE_DIRS := . include patch/include linux/include linux/arch/arm64/include linux/tools/arch/arm64/include

INCLUDE_FLAGS := $(foreach dir,$(INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(dir))

objs := xperia_ii_battery_age.o

all: xperia_ii_battery_age_$(MYKPM_VERSION).kpm

debug_objs := $(addprefix debug/,${objs})

debug: CFLAGS += -DDEBUG
debug: XIIBA_VER := _d
debug: xperia_ii_battery_age_$(MYKPM_VERSION)_debug.kpm

xperia_ii_battery_age_$(MYKPM_VERSION).kpm: ${objs}
	${CC} -r -o $@ $^

# AUD-c-1：debug 用独立对象目录，否则会复用 release 的 .o，两个产物逐字节相同
xperia_ii_battery_age_$(MYKPM_VERSION)_debug.kpm: ${debug_objs}
	${CC} -r -o $@ $^

%.o: %.c
	${CC} $(CFLAGS) $(INCLUDE_FLAGS) -c -o $@ $<

debug/%.o: %.c
	@mkdir -p $(@D)
	${CC} $(CFLAGS) $(INCLUDE_FLAGS) -c -o $@ $<

.PHONY: clean
clean:
	rm -rf *.kpm
	find . -name "*.o" | xargs rm -f
