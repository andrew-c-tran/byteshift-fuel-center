/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
#include "fw_crc32.h"
/* nibble table: 64 bytes of flash, ~2 M cycles for the 128 kB S32K116 flash at 48 MHz */
static const uint32_t T[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu };
uint32_t fw_crc32(const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t c = 0xFFFFFFFFu;
    while (len--) {
        c ^= *p++;
        c = T[c & 0xFu] ^ (c >> 4);
        c = T[c & 0xFu] ^ (c >> 4);
    }
    return c ^ 0xFFFFFFFFu;
}
