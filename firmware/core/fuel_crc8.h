/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
#ifndef FUEL_CRC8_H
#define FUEL_CRC8_H
#include <stdint.h>
/* CRC-8: poly 0x1D, init 0xFF, no reflection, no final XOR.
 * skip = index of the checksum byte, excluded from the calculation. */
uint8_t fuel_crc8(const uint8_t *buf, uint8_t len, uint8_t skip, uint8_t init);
#endif
