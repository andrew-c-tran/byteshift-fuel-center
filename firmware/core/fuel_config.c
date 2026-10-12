/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
#include "fuel_config.h"
#include "fuel_crc8.h"
#include "fuel_frame.h"

void fuel_cfg_default(fuel_cfg_t *c) { c->fitted = CFG_FIT_ALL; }

static bool mask_ok(uint8_t m, uint8_t inv) { return (m & ~CFG_FIT_ALL) == 0 && (uint8_t)~m == inv; }

bool fuel_cfg_decode_record(const uint8_t rec[8], fuel_cfg_t *c)
{
    if (rec[0] != 'F' || rec[1] != 'C' || rec[2] != CFG_REC_VERSION) return false;
    if (!mask_ok(rec[3], rec[4]) || rec[5] != 0 || rec[6] != 0) return false;
    if (fuel_crc8(rec, 8, 7, FRAME_CRC_INIT) != rec[7]) return false;
    c->fitted = rec[3];
    return true;
}

void fuel_cfg_encode_record(const fuel_cfg_t *c, uint8_t rec[8])
{
    rec[0] = 'F'; rec[1] = 'C'; rec[2] = CFG_REC_VERSION;
    rec[3] = c->fitted & CFG_FIT_ALL; rec[4] = (uint8_t)~rec[3]; rec[5] = 0; rec[6] = 0;
    rec[7] = fuel_crc8(rec, 8, 7, FRAME_CRC_INIT);
}

bool fuel_cfg_parse_frame(const uint8_t d[8], fuel_cfg_t *c)
{
    if (d[0] != CFG_FRAME_MAGIC) return false;
    if (!mask_ok(d[1], d[2]) || d[3] || d[4] || d[5] || d[6]) return false;
    if (fuel_crc8(d, 8, 7, FRAME_CRC_INIT) != d[7]) return false;
    c->fitted = d[1];
    return true;
}
