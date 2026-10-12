/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
#ifndef FLEX_DECODE_H
#define FLEX_DECODE_H
#include <stdint.h>
#include <stdbool.h>
/* GM 13577429 / Continental SE1004S decoder.
 * Feed it timestamps (microseconds, free-running 32-bit) of each edge on the sensor line
 * AFTER the input buffer, i.e. logical level as on the wire: idle HIGH, sensor pulls LOW.
 * Ethanol % = frequency - 50 (50..150 Hz). Fuel temp = LOW pulse width, 1 ms=-40C, 5 ms=125C. */

#define FLEX_FREQ_MIN_HZ        45.0f   /* GM P0178 threshold */
#define FLEX_FREQ_MAX_HZ       155.0f   /* rusEFI upper valid; above = fault */
#define FLEX_FREQ_CONTAM_LO_HZ 165.0f   /* 170-190 Hz = contaminated / self-diagnosed fault */
#define FLEX_FREQ_CONTAM_HI_HZ 200.0f
#define FLEX_PW_MIN_US          900u    /* rusEFI validity window for the temp pulse */
#define FLEX_PW_MAX_US         5100u
#define FLEX_TIMEOUT_US      100000u    /* no edge for 100 ms = no signal (5 periods at 50 Hz) */
#define FLEX_GLITCH_US           50u    /* ignore pulses shorter than this */
#define FLEX_MEDIAN_N             5     /* median-of-5 on period and pulse width */

typedef enum {
    FLEX_OK = 0,
    FLEX_NO_SIGNAL,        /* no edges within FLEX_TIMEOUT_US, or fewer than N periods since reset */
    FLEX_FREQ_LOW,         /* < 45 Hz: short / bad sensor */
    FLEX_FREQ_HIGH,        /* 155..165 or > 200 Hz: undefined, treat as fault */
    FLEX_CONTAMINATED,     /* 165..200 Hz: sensor says contaminated fuel or internal fault */
    FLEX_PW_BAD            /* reserved; pulse-width validity is reported via temp_ok */
} flex_status_t;

typedef struct {
    /* edge capture state */
    uint32_t last_fall_us, last_rise_us;
    bool     have_fall, have_rise;
    uint32_t period_buf[FLEX_MEDIAN_N], pw_buf[FLEX_MEDIAN_N];
    uint8_t  idx, count;
    /* results */
    float    freq_hz;      /* median frequency */
    float    pw_ms;        /* median low pulse width */
    float    ethanol_pct;  /* 0..100, valid only when status == FLEX_OK or CONTAM (then meaningless) */
    float    fuel_temp_c;  /* valid only when temp_ok */
    bool     temp_ok;
    flex_status_t status;
} flex_t;

void flex_init(flex_t *f);
/* Call from the input-capture ISR (or from a polled edge detector). level_after_edge: true = rising edge. */
void flex_edge(flex_t *f, uint32_t t_us, bool rising);
/* Call periodically (e.g. every 10 ms) with the current time; updates status/timeout and the decoded values. */
void flex_update(flex_t *f, uint32_t now_us);
#endif
