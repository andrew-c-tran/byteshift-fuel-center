/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
#ifndef FW_CRC32_H
#define FW_CRC32_H
#include <stdint.h>
/* CRC-32 (IEEE 802.3 / zlib): reflected poly 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF.
 * Used for the flash image check; tools/sign_image.py computes the same value with zlib.crc32. */
uint32_t fw_crc32(const void *buf, uint32_t len);
#endif
