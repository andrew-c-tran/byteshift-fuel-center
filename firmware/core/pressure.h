/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
#ifndef PRESSURE_H
#define PRESSURE_H
#include <stdint.h>
#include <stdbool.h>
/* Honeywell MIPAN2XX150PSAAX: ratiometric 10-90 % Vs = 0-150 psig (0-1034 kPa).
 * Fault rails: 2.5 % Vs (under-pressure) and 97.5 % Vs (internal fault / over-pressure).
 * A pull-down on the signal makes an open wire or dead sensor read ~0 V.
 * Both inputs are given as millivolts so the caller can use any ADC/divider; measure Vs (the 5 V
 * sensor rail) with the same ADC so the result is truly ratiometric. */

#define PRESS_VALID_LO_FRAC   0.05f   /* below 5 % Vs = fault (nominal min is 10 %) */
#define PRESS_VALID_HI_FRAC   0.95f   /* above 95 % Vs = fault (nominal max is 90 %) */
/* Absolute plausibility window for the sensor rail. Wide on purpose: the mV figures come from the MCU bandgap (+/-3 %)
 * and the rail is +/-3 %, so a healthy board can read 4.70-5.30 V. Ratiometric accuracy does not depend on this; the
 * app additionally checks that 5V_SENS tracks VDD to within 5 %, which is bandgap-independent. */
#define PRESS_VS_MIN_MV       4500
#define PRESS_VS_MAX_MV       5500
#define PRESS_NEG_LIMIT_KPA   -14.0f  /* below -2 psi is not a real rail pressure */
#define PRESS_IMPLAUSIBLE_KPA 830.0f  /* > 120 psi sustained on a PFI system */
#define PRESS_STEP_LIMIT_KPA  250.0f  /* > ~36 psi jump between consecutive filtered 10 ms samples */
#define PRESS_MEDIAN_N        5

typedef enum { PRESS_OK = 0, PRESS_NOT_READY, PRESS_VS_BAD, PRESS_SIG_LOW, PRESS_SIG_HIGH, PRESS_NEGATIVE, PRESS_STEP } press_status_t;

typedef struct {
    uint16_t buf_mv[PRESS_MEDIAN_N];
    uint8_t  idx, count;
    float    kpa;          /* filtered, valid when status == PRESS_OK */
    float    kpa_prev;
    bool     have_prev;
    bool     implausible;  /* > PRESS_IMPLAUSIBLE_KPA, value still reported */
    press_status_t status;
} press_t;

void  press_init(press_t *p);
/* Call every sample period (10 ms suggested; average several ADC conversions into sig_mv first). */
void  press_update(press_t *p, uint16_t sig_mv, uint16_t vs_mv);
#endif
