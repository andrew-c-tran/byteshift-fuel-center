/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
#include "fuel_frame.h"
#include "fuel_crc8.h"
#include <string.h>

void frame_init(frame_state_t *s) { memset(s, 0, sizeof(*s)); s->fitted = CFG_FIT_ALL; }

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline bool held(uint32_t now, uint32_t until) { return (int32_t)(until - now) > 0; }

void frame_build(frame_state_t *s, uint32_t now_ms, const flex_t *flex, const press_t *press,
                 bool module_fault, uint8_t out[8])
{
    uint32_t dt_ms = s->t_ms ? (now_ms - s->t_ms) : FUEL_FRAME_PERIOD_MS;
    if (dt_ms > FRAME_DT_CAP_MS) dt_ms = FRAME_DT_CAP_MS;   /* a stalled scheduler must not become one big ethanol jump */
    s->t_ms = now_ms;
    uint8_t st = 0;
    bool flex_fitted = (s->fitted & CFG_FIT_FLEX) != 0, press_fitted = (s->fitted & CFG_FIT_PRESS) != 0;

    /* --- ethanol --- */
    bool eth_bad = (flex->status != FLEX_OK);
    if (eth_bad) s->eth_err_until = now_ms + FRAME_ERR_HOLD_MS;
    if (flex->status == FLEX_CONTAMINATED) s->contam_until = now_ms + FRAME_ERR_HOLD_MS;
    bool eth_err = held(now_ms, s->eth_err_until);
    if (held(now_ms, s->contam_until)) st |= ST_ETH_CONTAM;

    /* stability tracking follows the RAW validity so a recovered sensor can seed as soon as the sticky error clears */
    float raw = flex->ethanol_pct;
    if (eth_bad) { s->stable_since_ms = 0; }
    else if (s->stable_since_ms == 0) { s->stable_since_ms = now_ms ? now_ms : 1; s->stable_min = s->stable_max = raw; }
    else {
        if (raw < s->stable_min) s->stable_min = raw;
        if (raw > s->stable_max) s->stable_max = raw;
        if (s->stable_max - s->stable_min > FRAME_SEED_SPREAD_PCT) { s->stable_since_ms = now_ms ? now_ms : 1; s->stable_min = s->stable_max = raw; }
    }
    bool stable = s->stable_since_ms && (now_ms - s->stable_since_ms) >= FRAME_SEED_MS;
    bool long_stable = s->stable_since_ms && (now_ms - s->stable_since_ms) >= FRAME_RESEED_MS;

    float eth_out = 0.0f;
    if (!flex_fitted) {
        s->eth_have = false; s->eth_reported = 0.0f; s->stable_since_ms = 0; s->eth_err_until = s->contam_until = 0;
        st &= (uint8_t)~ST_ETH_CONTAM;       /* not fitted: 0 %, no flags */
    } else if (eth_err) {
        st |= ST_ETH_ERR;
        s->eth_have = false;                 /* after a fault, recovery restarts from 0 until the reading is stable */
        s->eth_reported = 0.0f;
    } else {
        if (!s->eth_have && stable) { s->eth_reported = raw; s->eth_have = true; }   /* seed once */
        else if (s->eth_have && long_stable && raw > s->eth_reported) {
            s->eth_reported = raw;                                                  /* re-seed: steady for 5 s */
            s->stable_since_ms = now_ms ? now_ms : 1; s->stable_min = s->stable_max = raw;
        }
        if (s->eth_have && raw <= s->eth_reported) {
            s->eth_reported = raw;           /* downward: follow immediately */
        } else {
            float max_step = FRAME_ETH_UP_PCT_PER_S * (float)dt_ms / 1000.0f;
            if (raw - s->eth_reported > max_step) { s->eth_reported += max_step; st |= ST_ETH_SLEW; }
            else s->eth_reported = raw;
        }
        eth_out = s->eth_reported;
    }

    /* --- fuel temperature --- */
    bool temp_bad = !flex->temp_ok;
    if (temp_bad) s->temp_err_until = now_ms + FRAME_ERR_HOLD_MS;
    bool temp_err = held(now_ms, s->temp_err_until);
    float temp_out = temp_err ? 125.0f : flex->fuel_temp_c;
    if (!flex_fitted) { temp_out = 125.0f; s->temp_err_until = 0; }
    else if (temp_err) st |= ST_TEMP_ERR;

    /* --- pressure --- */
    bool press_bad = (press->status != PRESS_OK);
    if (press_bad) s->press_err_until = now_ms + FRAME_ERR_HOLD_MS;
    bool press_err = held(now_ms, s->press_err_until);
    float kpa_out = press_err ? 0.0f : press->kpa;
    if (!press_fitted) { kpa_out = 0.0f; s->press_err_until = 0; }
    else if (press_err) st |= ST_PRESS_ERR;
    else if (press->implausible) st |= ST_PRESS_IMPL;

    /* --- module: a module that distrusts itself sends the fail-safe values too, the sensor bits stay as measured --- */
    if (module_fault) { st |= ST_MODULE_FAULT; eth_out = 0.0f; kpa_out = 0.0f; temp_out = 125.0f; }
    if (now_ms < FRAME_STARTUP_MS) { st |= ST_NOT_READY; eth_out = 0.0f; kpa_out = 0.0f; temp_out = 125.0f; }

    /* --- pack --- */
    eth_out  = clampf(eth_out, 0.0f, 100.0f);
    temp_out = clampf(temp_out, -40.0f, 125.0f);
    kpa_out  = clampf(kpa_out, 0.0f, 65535.0f);
    uint16_t kpa = (uint16_t)(kpa_out + 0.5f);
    float fhz = flex_fitted ? clampf(flex->freq_hz, 0.0f, 255.0f) : 0.0f;

    out[0] = (uint8_t)(eth_out + 0.5f);
    out[1] = (uint8_t)(temp_out + 40.0f + 0.5f);
    out[2] = (uint8_t)(kpa >> 8);
    out[3] = (uint8_t)(kpa & 0xFF);
    out[4] = st;
    out[5] = (uint8_t)((s->counter << 4) | (FRAME_PROTO_VERSION & 0x0F));
    out[6] = (uint8_t)(fhz + 0.5f);
    out[7] = fuel_crc8(out, 8, 7, FRAME_CRC_INIT);
    s->counter = (uint8_t)((s->counter + 1) & 0x0F);
}
