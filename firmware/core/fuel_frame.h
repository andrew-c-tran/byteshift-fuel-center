/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
#ifndef FUEL_FRAME_H
#define FUEL_FRAME_H
#include <stdint.h>
#include <stdbool.h>
#include "flex_decode.h"
#include "pressure.h"
#include "fuel_config.h"
/* Builds the 0x5A6 frame (protocol version 2). Applies:
 *  - sticky error bits (hold >= FRAME_ERR_HOLD_MS after the last bad sample)
 *  - fail-safe substitution (ethanol 0 %, pressure 0 kPa, temp 125 C) whenever the matching error bit is set
 *  - asymmetric ethanol slew limit: unlimited downward, FRAME_ETH_UP_PCT_PER_S upward
 *  - not-ready bit for the first FRAME_STARTUP_MS
 *  - module_fault: bit 1 AND the fail-safe values (the module does not trust its own measurements)
 *  - channels not fitted (frame_state_t.fitted): fail-safe value, no error bit
 *  - rolling counter, protocol version, CRC-8 in byte 7 */

#define FUEL_FRAME_ID          0x5A6u
#define FUEL_FRAME_PERIOD_MS   10u
#define FRAME_PROTO_VERSION    2u
#define FRAME_ERR_HOLD_MS      1000u
#define FRAME_STARTUP_MS       500u
#define FRAME_ETH_UP_PCT_PER_S 3.0f
#define FRAME_DT_CAP_MS        100u
/* Seeding: after power-up or after an ethanol fault, the reported value starts at 0 and ramps up at the slew
 * rate UNLESS the raw reading has been valid and stable (spread <= FRAME_SEED_SPREAD_PCT) for FRAME_SEED_MS,
 * in which case the reported value jumps to that stable reading once. This avoids leaning out an E85 car for
 * ~30 s after every key-on while still refusing to follow a sudden implausible rise during operation. */
#define FRAME_SEED_MS          1000u
#define FRAME_SEED_SPREAD_PCT  3.0f
/* Re-seeding: once seeded, if the raw reading has held within FRAME_SEED_SPREAD_PCT for FRAME_RESEED_MS and
 * differs from the reported value, jump to it. A steady reading is the sensor's honest answer (contamination,
 * methanol and noise are caught elsewhere), so this only shortens the delay for real changes, e.g. a sensor
 * that read air at key-on and filled with E85 during pump prime. */
#define FRAME_RESEED_MS        5000u
#define FRAME_CRC_INIT         0xFFu

#define ST_ETH_ERR      (1u << 7)
#define ST_TEMP_ERR     (1u << 6)
#define ST_PRESS_ERR    (1u << 5)
#define ST_ETH_CONTAM   (1u << 4)
#define ST_ETH_SLEW     (1u << 3)
#define ST_PRESS_IMPL   (1u << 2)
#define ST_MODULE_FAULT (1u << 1)
#define ST_NOT_READY    (1u << 0)

typedef struct {
    uint32_t t_ms;            /* time of last build */
    uint32_t eth_err_until, temp_err_until, press_err_until, contam_until;
    float    eth_reported;    /* slew-limited value */
    bool     eth_have;        /* true once seeded; slew limit applies from then on */
    uint32_t stable_since_ms; /* start of the current stable window (0 = none) */
    float    stable_min, stable_max;
    uint8_t  counter;
    uint8_t  fitted;          /* CFG_FIT_* mask; frame_init sets CFG_FIT_ALL, the app applies the stored configuration */
} frame_state_t;

void frame_init(frame_state_t *s);
/* module_fault: caller's own self-test result (5 V ref, supply, watchdog reset flag...) */
void frame_build(frame_state_t *s, uint32_t now_ms, const flex_t *flex, const press_t *press,
                 bool module_fault, uint8_t out[8]);
#endif
