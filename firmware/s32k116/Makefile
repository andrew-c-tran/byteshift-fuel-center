# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ByteShift Labs LLC
# Cross build for the S32K116 target: make -C firmware/s32k116 [TOOLCHAIN=/path/to/arm-gnu-toolchain/bin]
TOOLCHAIN ?=
PREFIX := $(if $(TOOLCHAIN),$(TOOLCHAIN)/)arm-none-eabi-
CC := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
SIZE := $(PREFIX)size
# RAM-resident sections the NXP linker script marks loadable; they must not land in the bin/hex (or the CRC)
NORAM := -R .interrupts_ram -R .heap -R .stack
CORE := ../core/fuel_crc8.c ../core/fw_crc32.c ../core/flex_decode.c ../core/pressure.c ../core/fuel_frame.c ../core/fuel_config.c
SRC := $(CORE) ../app/main.c bsp_s32k116.c nxp/system_S32K116.c nxp/startup.c nxp/startup_S32K116.S main_entry.c
CFLAGS := -mcpu=cortex-m0plus -mthumb -std=c99 -Os -g3 -Wall -Wextra -ffunction-sections -fdata-sections -fno-common \
          -DCPU_S32K116 -DSTART_FROM_FLASH -I../core -I../app -Inxp
LDFLAGS := -mcpu=cortex-m0plus -mthumb -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections -Wl,-Map=build/fuel-center.map -T nxp/S32K116_17_flash.ld -Wl,--defsym=__stack_size__=0x400
# The image is signed: CRC-32 over flash 0x00000..0x1FFFB is written into the .fw_crc word at 0x1FFFC (see
# tools/sign_image.py). The BSP recomputes it at boot; an unsigned image (word 0xFFFFFFFF) flags MODULE_FAULT.
all: build/fuel-center.elf build/fuel-center.hex build/fuel-center.bin
	$(SIZE) build/fuel-center.elf
build/fuel-center-unsigned.elf: $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
build/fuel-center.elf: build/fuel-center-unsigned.elf ../tools/sign_image.py
	$(OBJCOPY) $(NORAM) -O ihex $< build/fuel-center-unsigned.hex
	python3 ../tools/sign_image.py build/fuel-center-unsigned.hex build/fw_crc.bin
	$(OBJCOPY) --update-section .fw_crc=build/fw_crc.bin $< $@
build/fuel-center.hex: build/fuel-center.elf
	$(OBJCOPY) $(NORAM) -O ihex $< $@
	python3 ../tools/sign_image.py --verify $@
build/fuel-center.bin: build/fuel-center.hex
	python3 ../tools/sign_image.py --to-bin $< $@
clean:
	rm -rf build
.PHONY: all clean
