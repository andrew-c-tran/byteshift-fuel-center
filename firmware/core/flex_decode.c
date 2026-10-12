/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
#include "flex_decode.h"
#include <string.h>

static uint32_t median_u32(const uint32_t *in, uint8_t n)
{
    uint32_t a[FLEX_MEDIAN_N];
    memcpy(a, in, n * sizeof(uint32_t));
    for (uint8_t i = 1; i < n; i++) {             /* insertion sort, n <= 5 */
        uint32_t v = a[i]; int8_t j = (int8_t)i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
    return a[n / 2];
}

void flex_init(flex_t *f)
{
    memset(f, 0, sizeof(*f));
    f->status = FLEX_NO_SIGNAL;
}

void flex_edge(flex_t *f, uint32_t t_us, bool rising)
{
    if (rising) {
        /* end of the LOW pulse: pulse width = rise - fall */
        if (f->have_fall) {
            uint32_t pw = t_us - f->last_fall_us;
            if (pw < FLEX_GLITCH_US) return;             /* glitch, ignore */
            f->pw_buf[f->idx] = pw;
            f->have_rise = true;
        }
        f->last_rise_us = t_us;
    } else {
        /* falling edge = start of a new period */
        if (f->have_fall) {
            uint32_t period = t_us - f->last_fall_us;
            if (period < FLEX_GLITCH_US) return;
            /* A complete sensor cycle has both edges. Without the rise, old pulse widths
             * must not be paired with new periods and presented as valid readings. */
            f->period_buf[f->idx] = f->have_rise ? period : 0;
            if (!f->have_rise) f->pw_buf[f->idx] = 0;
            f->idx = (uint8_t)((f->idx + 1) % FLEX_MEDIAN_N);
            if (f->count < FLEX_MEDIAN_N) f->count++;
        }
        f->last_fall_us = t_us; f->have_fall = true; f->have_rise = false;
    }
}

void flex_update(flex_t *f, uint32_t now_us)
{
    uint32_t last = f->have_fall ? f->last_fall_us : now_us - FLEX_TIMEOUT_US - 1;
    if (!f->have_fall || (uint32_t)(now_us - last) > FLEX_TIMEOUT_US) {
        /* signal lost: throw away history so a recovery needs N fresh periods */
        f->count = 0; f->idx = 0; f->have_fall = false; f->have_rise = false;
        f->status = FLEX_NO_SIGNAL; f->temp_ok = false; f->freq_hz = 0.0f;
        return;
    }
    if (f->count < FLEX_MEDIAN_N) { f->status = FLEX_NO_SIGNAL; f->temp_ok = false; return; }

    uint32_t period = median_u32(f->period_buf, FLEX_MEDIAN_N);
    uint32_t pw     = median_u32(f->pw_buf, FLEX_MEDIAN_N);
    f->freq_hz = period ? 1.0e6f / (float)period : 0.0f;
    f->pw_ms   = (float)pw / 1000.0f;

    if (f->freq_hz < FLEX_FREQ_MIN_HZ)                 f->status = FLEX_FREQ_LOW;
    else if (f->freq_hz <= FLEX_FREQ_MAX_HZ)           f->status = FLEX_OK;
    else if (f->freq_hz >= FLEX_FREQ_CONTAM_LO_HZ &&
             f->freq_hz <= FLEX_FREQ_CONTAM_HI_HZ)     f->status = FLEX_CONTAMINATED;
    else                                               f->status = FLEX_FREQ_HIGH;

    f->temp_ok = (pw >= FLEX_PW_MIN_US && pw <= FLEX_PW_MAX_US);
    if (f->status == FLEX_OK) {
        float e = f->freq_hz - 50.0f;
        f->ethanol_pct = e < 0.0f ? 0.0f : (e > 100.0f ? 100.0f : e);
    } else {
        f->ethanol_pct = 0.0f;
    }
    if (f->temp_ok && (f->status == FLEX_OK || f->status == FLEX_CONTAMINATED)) {
        f->fuel_temp_c = 41.25f * f->pw_ms - 81.25f;
    } else {
        f->temp_ok = false;
    }
}
