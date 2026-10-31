MYKPM_VERSION := 0.0.1

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

CFLAGS = -Wall -O2 -fno-PIC -fno-asynchronous-unwind-tables -fno-stack-protector -fno-common -DMYKPM_VERSION=\"$(MYKPM_VERSION)$(MC_VER)\"

INCLUDE_DIRS := . include patch/include linux/include linux/arch/arm64/include linux/tools/arch/arm64/include

INCLUDE_FLAGS := $(foreach dir,$(INCLUDE_DIRS),-I$(KP_DIR)/kernel/$(dir))
INCLUDE_FLAGS += -I. -Icontrol -Ikomm -Iredirect -Ifeature -Idata -Iptrace

C_FILES := mod_main.c
C_FILES += control/control.c
C_FILES += komm/komm.c
C_FILES += redirect/redirect.c
C_FILES += feature/feature.c
C_FILES += data/shared_data.c
C_FILES += data/kstring.c
C_FILES += data/parser.c
C_FILES += ptrace/ptrace_hide.c

objs := $(C_FILES:.c=.o)
debug_objs := $(addprefix debug/,$(objs))

all: memcrc_$(MYKPM_VERSION).kpm

debug: CFLAGS += -DCONFIG_DEBUG
debug: MC_VER := _d
debug: memcrc_$(MYKPM_VERSION)_debug.kpm

memcrc_$(MYKPM_VERSION).kpm: ${objs}
	${CC} -r -o $@ $^

memcrc_$(MYKPM_VERSION)_debug.kpm: ${debug_objs}
	@mkdir -p debug
	${CC} -r -o $@ $^

debug/%.o: %.c
	@mkdir -p $(@D)
	${CC} $(CFLAGS) $(INCLUDE_FLAGS) -c -o $@ $<

%.o: %.c
	${CC} $(CFLAGS) $(INCLUDE_FLAGS) -c -o $@ $<

.PHONY: clean
clean:
	rm -rf *.kpm debug
	find . -name "*.o" | xargs rm -f
