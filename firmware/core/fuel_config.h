/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
#ifndef FUEL_CONFIG_H
#define FUEL_CONFIG_H
#include <stdint.h>
#include <stdbool.h>
/* Installer configuration: which sensors are fitted. A channel that is not fitted sends its fail-safe value
 * (0 %, 125 C, 0 kPa) permanently but WITHOUT the error bit, so the receiver can tell "absent" from "broken".
 * Default (blank or corrupt flash record): both fitted, i.e. a missing sensor is reported as a fault.
 * Set over CAN with the 0x5A8 frame; stored as one 8-byte record in data flash (see docs/protocol.md). */

#define CFG_FRAME_ID     0x5A8u
#define CFG_FRAME_MAGIC  0xC5u
#define CFG_FIT_FLEX     0x01u   /* GM 13577429 flex fuel sensor fitted */
#define CFG_FIT_PRESS    0x02u   /* H-PS150G-HMIP pressure sensor fitted */
#define CFG_FIT_ALL      (CFG_FIT_FLEX | CFG_FIT_PRESS)
#define CFG_REC_VERSION  1u

typedef struct { uint8_t fitted; } fuel_cfg_t;

void fuel_cfg_default(fuel_cfg_t *c);
/* Flash record: 'F' 'C' version fitted ~fitted 0 0 CRC-8(bytes 0-6). Returns false for blank/corrupt (c untouched). */
bool fuel_cfg_decode_record(const uint8_t rec[8], fuel_cfg_t *c);
void fuel_cfg_encode_record(const fuel_cfg_t *c, uint8_t rec[8]);
/* CAN 0x5A8: A = 0xC5, B = fitted mask, C = ~B, D-G = 0, H = CRC-8. Returns false (c untouched) if anything is off. */
bool fuel_cfg_parse_frame(const uint8_t d[8], fuel_cfg_t *c);
#endif
