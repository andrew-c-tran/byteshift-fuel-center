/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
/* Host simulator BSP: drives the app with a scripted scenario and prints every CAN frame as candump text.
 * Build: make -C firmware sim ; run: ./build/sim [scenario]  */
#include "bsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fuel_crc8.h"

static uint64_t t_us;                 /* simulated time */
static double flex_hz = 50.0, flex_pw_ms = 2.45;   /* key-on: sensor drained, reads air = 0 % */ static bool flex_present = true;
static double p_mv = 2337, vs_mv = 5000, vdd_mv = 5000, vbat_mv = 13800; static bool adc_ok = true;
static uint64_t next_fall_us = 1000;
static bool in_low; static uint64_t rise_at;

static void scenario_step(uint32_t ms)
{
    /* default scenario: healthy -> ethanol jump up -> contaminated -> recovery -> pressure open wire -> Vs sag */
    if (ms == 2000)  { flex_hz = 135; }                /* pump prime fills the sensor with E85 */
    if (ms == 12000) { flex_hz = 60; }                 /* drop to 10 % (immediate) */
    if (ms == 14000) { flex_hz = 135; }                /* back up to 85 %: ramps at 3 %/s, re-seeds after 5 s steady */
    if (ms == 40000) { flex_hz = 180; }                /* contaminated */
    if (ms == 42000) { flex_hz = 135; }
    if (ms == 45000) { flex_present = false; }         /* open wire */
    if (ms == 47000) { flex_present = true; }
    if (ms == 50000) { p_mv = 0; }                     /* pressure open wire (pull-down) */
    if (ms == 52000) { p_mv = 2337; }
    if (ms == 54000) { vs_mv = 4500; }                 /* sensor rail sag */
    if (ms == 56000) { vs_mv = 5000; }
    if (ms == 58000) { p_mv = 4875; }                  /* sensor fault rail */
    if (ms == 60000) { p_mv = 2337; }
}

void bsp_init(void) {}
uint32_t bsp_millis(void)
{
    /* advance simulated time by 1 ms per call and emit flex edges that fall inside the step */
    uint32_t ms_before = (uint32_t)(t_us / 1000);
    t_us += 1000;
    scenario_step(ms_before);
    if (flex_present) {
        while (next_fall_us <= t_us || (in_low && rise_at <= t_us)) {
            if (in_low && rise_at <= t_us && rise_at <= next_fall_us) { app_flex_edge((uint32_t)rise_at, true); in_low = false; continue; }
            if (next_fall_us <= t_us) {
                app_flex_edge((uint32_t)next_fall_us, false); in_low = true;
                rise_at = next_fall_us + (uint64_t)(flex_pw_ms * 1000.0);
                next_fall_us += (uint64_t)(1e6 / flex_hz);
            }
        }
    } else { next_fall_us = t_us + 1000; in_low = false; }
    return (uint32_t)(t_us / 1000);
}
uint32_t bsp_micros(void) { return (uint32_t)t_us; }
void bsp_wdog_kick(void) {}
bool bsp_reset_was_watchdog(void) { return false; }
bool bsp_adc_read(bsp_adc_t *o)
{
    o->p_sig_mv = (uint16_t)p_mv; o->vs_sens_mv = (uint16_t)vs_mv; o->vbat_mv = (uint16_t)vbat_mv; o->vdd_mv = (uint16_t)vdd_mv; o->board_temp_c = 35; o->adc_ok = adc_ok;
    return adc_ok;
}
static uint32_t last_print_ms = 0;
bool bsp_can_send(uint16_t id, const uint8_t d[8])
{
    uint32_t ms = (uint32_t)(t_us / 1000);
    if (id == 0x5A6 && (ms - last_print_ms < 500) && ms > 600) return true;   /* print 2 frames/s to keep output short */
    if (id == 0x5A6) last_print_ms = ms;
    if (id != 0x5A6) return true;
    printf("(%6u.%03u) can0 %03X#%02X%02X%02X%02X%02X%02X%02X%02X  eth=%3u%% T=%4d C P=%4u kPa st=%02X cnt=%u f=%u Hz\n",
           ms / 1000, ms % 1000, id, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
           d[0], (int)d[1] - 40, (d[2] << 8) | d[3], d[4], d[5] >> 4, d[6]);
    return true;
}
bool bsp_can_healthy(void) { return true; }
uint32_t bsp_can_tx_errors(void) { return 0; }
uint32_t bsp_can_rx_errors(void) { return 0; }
uint32_t bsp_can_reinits(void) { return 0; }
/* Config: FUELCAN_FITTED=<mask> in the environment plays the stored flash record (1 flex only, 2 pressure only, 3 both).
 * FUELCAN_CFG_AT_MS=<ms> additionally injects a 0x5A8 frame with that mask at the given time. */
#include "fuel_config.h"
bool bsp_config_read(uint8_t rec[8])
{
    const char *e = getenv("FUELCAN_FITTED"); if (!e) return false;
    fuel_cfg_t c = { (uint8_t)atoi(e) }; fuel_cfg_encode_record(&c, rec); return true;
}
bool bsp_config_write(const uint8_t rec[8]) { fprintf(stderr, "config stored: fitted=%u\n", rec[3]); return true; }
bool bsp_can_recv(uint16_t *id, uint8_t data[8])
{
    static bool sent; const char *at = getenv("FUELCAN_CFG_AT_MS"), *m = getenv("FUELCAN_FITTED");
    if (sent || !at || !m || t_us / 1000 < (uint64_t)atoi(at)) return false;
    sent = true; *id = CFG_FRAME_ID;
    memset(data, 0, 8); data[0] = CFG_FRAME_MAGIC; data[1] = (uint8_t)atoi(m); data[2] = (uint8_t)~data[1];
    data[7] = fuel_crc8(data, 8, 7, 0xFF); return true;
}
uint32_t bsp_self_test_faults(void) { return 0; }

int app_main(void);
int main(void) { return app_main(); }
void bsp_debug_puts(const char *s) { fputs(s, stderr); }
