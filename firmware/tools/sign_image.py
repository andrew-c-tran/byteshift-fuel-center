#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ByteShift Labs LLC
"""Image signing for the S32K116 build. CRC-32 (zlib) over flash bytes 0x00000..0x1FFFB is stored little-endian in the
.fw_crc word at 0x1FFFC; the firmware (bsp_s32k116.c flash_check, core/fw_crc32.c) recomputes it at boot.
Works from Intel HEX because the NXP linker script leaves empty RAM sections that make `objcopy -O binary` span to 0x20000000.
  sign_image.py <unsigned.hex> <fw_crc.bin>   write the 4-byte CRC word for `objcopy --update-section .fw_crc=`
  sign_image.py --verify <signed.hex>         recompute and compare with the stored word (exit 1 on mismatch)
  sign_image.py --to-bin <signed.hex> <out.bin>   128 kB flash image, 0xFF where nothing is programmed"""
import sys, zlib, struct
CRC_ADDR = 0x1FFFC
FLASH_END = CRC_ADDR + 4

def read_hex(path):
    img = bytearray(b'\xff' * FLASH_END); base = 0; seen = bytearray(FLASH_END)
    for ln in open(path):
        ln = ln.strip()
        if not ln.startswith(':'): continue
        rec = bytes.fromhex(ln[1:])
        n, addr, typ, data = rec[0], (rec[1] << 8) | rec[2], rec[3], rec[4:-1]
        if (sum(rec) & 0xFF) != 0: raise SystemExit(f'{path}: bad checksum in record {ln}')
        if typ == 0x04: base = (data[0] << 24) | (data[1] << 16)
        elif typ == 0x02: base = ((data[0] << 8) | data[1]) << 4
        elif typ == 0x01: break
        elif typ == 0x00:
            a = base + addr
            if a + n > FLASH_END: raise SystemExit(f'{path}: record at 0x{a:X} is outside the 128 kB flash (RAM section in the hex?)')
            img[a:a + n] = data; seen[a:a + n] = b'\x01' * n
    if not seen[0]: raise SystemExit(f'{path}: no vector table at 0x0')
    return img

def crc_of(img): return zlib.crc32(img[:CRC_ADDR]) & 0xFFFFFFFF

a = sys.argv[1:]
if a[0] == '--verify':
    img = read_hex(a[1]); crc = crc_of(img); stored = struct.unpack_from('<I', img, CRC_ADDR)[0]
    print(f'image CRC-32 {crc:08X}, stored {stored:08X}: {"OK" if crc == stored else "MISMATCH"}')
    sys.exit(0 if crc == stored else 1)
if a[0] == '--to-bin':
    open(a[2], 'wb').write(read_hex(a[1])); sys.exit(0)
img = read_hex(a[0]); crc = crc_of(img)
open(a[1], 'wb').write(struct.pack('<I', crc))
print(f'image CRC-32 {crc:08X} over {CRC_ADDR} bytes -> {a[1]}')
