# TinyUF2 board for cubevox (STM32H743VIT6). boards/weact/board.mk sets CV_BOARD = weact and
# includes this file. The port includes board.mk twice, so the body runs once.
ifndef CV_BOARD_MK
CV_BOARD_MK := 1

CV_BOARD ?= cubevox
MCU = h743xx
JLINK_DEVICE = stm32h743vi

# The board directories are symlinks into the cubevox repository; real paths find its files.
CV_SRC := $(PORT_DIR)/boards/cubevox
CV_REAL := $(realpath $(TOP)/$(CV_SRC))
CV_BOOTLOADER := $(realpath $(CV_REAL)/../..)
CV_REPO := $(realpath $(CV_BOOTLOADER)/../..)
CV_BOOT_BLOCK_H := $(CV_REPO)/firmware/cubevox/src/board/boot_block.h

# A value from boot_block.h, which keeps every shared value an integer literal alone on its line.
# The hash sits in a variable because make 3.81 and 4.3 read \# in a function call differently.
CV_HASH := \#
cv_define = $(shell awk '$$1 == "$(CV_HASH)define" && $$2 == "$(1)" { sub(/[uU]$$/, "", $$3); print $$3 }' '$(CV_BOOT_BLOCK_H)')
CV_APP_END := $(call cv_define,CV_APP_END)
CV_BOOT_BLOCK_ADDR := $(call cv_define,CV_BOOT_BLOCK_ADDR)
CV_FAULT_RECORD_ADDR := $(call cv_define,CV_FAULT_RECORD_ADDR)
ifeq ($(CV_BOARD),weact)
CV_FAMILY := $(call cv_define,CV_UF2_FAMILY_WEACT)
CV_BOARD_WEACT := 1
else
CV_FAMILY := $(call cv_define,CV_UF2_FAMILY_CUBEVOX)
CV_BOARD_WEACT := 0
endif
$(foreach v,CV_APP_END CV_BOOT_BLOCK_ADDR CV_FAULT_RECORD_ADDR CV_FAMILY,\
  $(if $($(v)),,$(error $(v) not found in $(CV_BOOT_BLOCK_H))))

# The board's own UF2 family instead of the port's STM32H7 id, which port.mk assigns after this file.
override UF2_FAMILY_ID = $(CV_FAMILY)

# Pinned upstream commit and patch count for INFO_UF2.TXT.
CV_TINYUF2_VERSION := $(shell git -C '$(TOP)' describe --always --tags) with $(words $(wildcard $(CV_BOOTLOADER)/patches/*.patch)) cubevox patches

# HSE_VALUE is the 25 MHz crystal, kHseHz in firmware/cubevox/src/board/clocks.h.
CFLAGS += \
  -DSTM32H743xx \
  -DHSE_VALUE=25000000U \
  -DTINYUF2_LED=1 \
  -DBOARD_PFLASH_APP_EN=1 \
  -DCV_BOARD_WEACT=$(CV_BOARD_WEACT) \
  -DCV_TINYUF2_VERSION='"$(CV_TINYUF2_VERSION)"'

INC += \
  $(TOP)/$(CV_SRC) \
  $(CV_REPO)/firmware/cubevox/src/board \
  $(CV_REPO)/tools/font-editor/fonts

# NOINIT is the boot block below the fault record, so _board_dfu_dbl_tap is its request word. The
# application flash, for TinyUF2's own apps, ends where bank 2 begins.
LDFLAGS += \
  -Wl,--defsym=BOARD_NOINIT_ORIGIN=$(CV_BOOT_BLOCK_ADDR) \
  -Wl,--defsym=BOARD_NOINIT_SIZE=$(CV_FAULT_RECORD_ADDR)-$(CV_BOOT_BLOCK_ADDR) \
  -Wl,--defsym=BOARD_PFLASH_APP_END=$(CV_APP_END)

SRC_C += \
  $(CV_SRC)/board_hooks.c \
  $(CV_SRC)/boot_decide.c \
  $(CV_SRC)/clock.c \
  $(CV_SRC)/diag.c \
  $(CV_SRC)/image_check.c

SRC_S += \
  $(ST_CMSIS)/Source/Templates/gcc/startup_stm32h743xx.s

ifeq ($(CV_BOARD),cubevox)
SRC_C += \
  $(CV_SRC)/oled.c \
  $(CV_SRC)/screens.c

# fonts.cpp hands the project's C++ font header to the C code. The port's rules compile only C,
# and C-only warnings are dropped for C++.
CV_FONTS_OBJ := $(BUILD)/obj/$(CV_SRC)/fonts.o
OBJ += $(CV_FONTS_OBJ)
$(CV_FONTS_OBJ): $(TOP)/$(CV_SRC)/fonts.cpp
	@echo CXX $(notdir $@)
	@$(CROSS_COMPILE)g++ $(filter-out -Wstrict-prototypes -Werror-implicit-function-declaration,$(CFLAGS)) \
	  -std=c++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -c -o $@ $<
endif

endif
