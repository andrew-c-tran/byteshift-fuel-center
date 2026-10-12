/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
#include "pressure.h"
#include <string.h>

static uint16_t median_u16(const uint16_t *in, uint8_t n)
{
    uint16_t a[PRESS_MEDIAN_N];
    memcpy(a, in, n * sizeof(uint16_t));
    for (uint8_t i = 1; i < n; i++) { uint16_t v = a[i]; int8_t j = (int8_t)i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
    return a[n / 2];
}

void press_init(press_t *p) { memset(p, 0, sizeof(*p)); p->status = PRESS_NOT_READY; }

void press_update(press_t *p, uint16_t sig_mv, uint16_t vs_mv)
{
    p->buf_mv[p->idx] = sig_mv;
    p->idx = (uint8_t)((p->idx + 1) % PRESS_MEDIAN_N);
    if (p->count < PRESS_MEDIAN_N) p->count++;
    if (p->count < PRESS_MEDIAN_N) { p->status = PRESS_NOT_READY; return; }

    if (vs_mv < PRESS_VS_MIN_MV || vs_mv > PRESS_VS_MAX_MV) { p->status = PRESS_VS_BAD; p->have_prev = false; return; }

    float frac = (float)median_u16(p->buf_mv, PRESS_MEDIAN_N) / (float)vs_mv;
    if (frac < PRESS_VALID_LO_FRAC) { p->status = PRESS_SIG_LOW;  p->have_prev = false; return; }
    if (frac > PRESS_VALID_HI_FRAC) { p->status = PRESS_SIG_HIGH; p->have_prev = false; return; }

    float kpa = 1034.2f * (frac - 0.10f) / 0.80f;      /* 150 psi = 1034.2 kPa */
    if (kpa < PRESS_NEG_LIMIT_KPA) { p->status = PRESS_NEGATIVE; p->have_prev = false; return; }
    if (kpa < 0.0f) kpa = 0.0f;                          /* small negative from tolerance -> 0 */

    if (p->have_prev && (kpa - p->kpa_prev) > PRESS_STEP_LIMIT_KPA) {
        /* an upward jump this fast is not a fuel system event; downward jumps (pump death) are real and pass */
        p->status = PRESS_STEP; p->kpa_prev = kpa; return;
    }
    p->kpa_prev = kpa; p->have_prev = true;
    p->kpa = kpa;
    p->implausible = kpa > PRESS_IMPLAUSIBLE_KPA;
    p->status = PRESS_OK;
}
