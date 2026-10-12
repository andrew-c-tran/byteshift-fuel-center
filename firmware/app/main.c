/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
/* Application: 10 ms loop -> read pressure ADC, update decoders, build and send 0x5A6; 100 ms -> 0x5A7 diag. */
#include "bsp.h"
#include "flex_decode.h"
#include "pressure.h"
#include "fuel_frame.h"
#include "fuel_crc8.h"
#include "fuel_config.h"

#define FW_VERSION 0x03      /* 0x03: installer config (0x5A8, diag page 2), MODULE_FAULT substitutes values, wider 5 V windows */
#define DIAG_ID    0x5A7u

/* 0x5A7 page 1 byte C: status bits (latched for the life of this boot where noted) */
#define ST_WDOG_RESET  0x01u   /* this boot followed a watchdog reset (latched) */
#define ST_ADC         0x02u   /* ADC self-test (bandgap / VREFH / VREFL) out of window */
#define ST_VDD         0x04u   /* 5 V rail outside 4.5-5.5 V (bandgap-referenced, +/-3 % bandgap x +/-3 % LDO) */
#define ST_CAN         0x08u   /* bus off or a stalled TX mailbox */
#define ST_FLASH       0x10u   /* flash CRC mismatch or unsigned image (latched) */
#define ST_RAM         0x20u   /* RAM march test failed at boot (latched) */
#define ST_STACK       0x40u   /* stack canary overwritten (latched) */
#define ST_FAULT       0x80u   /* summary: MODULE_FAULT is being sent in 0x5A6 */

static flex_t g_flex;
static press_t g_press;
static frame_state_t g_frame;
static uint8_t g_status;
static fuel_cfg_t g_cfg;
static uint8_t g_cfg_src;      /* diag page 2 byte B: 0 defaults (blank flash), 1 flash record, 2 corrupt record -> defaults */
static uint8_t g_cfg_result;   /* diag page 2 byte C: last 0x5A8 handled: 0 none, 1 stored, 2 rejected frame, 3 flash write failed */
static uint8_t g_cfg_writes;   /* diag page 2 byte D: accepted 0x5A8 frames since boot */

void app_flex_edge(uint32_t t_us, bool rising) { flex_edge(&g_flex, t_us, rising); }

/* Everything that can make the module distrust itself. Returns true -> MODULE_FAULT bit in 0x5A6 and fail-safe values. */
static bool module_self_test(const bsp_adc_t *adc, uint32_t now_ms, uint32_t *wd_until)
{
    uint8_t st = g_status & (ST_WDOG_RESET | ST_FLASH | ST_RAM | ST_STACK);   /* latched bits survive */
    if (bsp_reset_was_watchdog()) { st |= ST_WDOG_RESET; if (*wd_until == 0) *wd_until = now_ms + 5000; }
    bool fault = (*wd_until != 0 && now_ms < *wd_until);
    if (!adc->adc_ok) st |= ST_ADC;
    if (adc->vdd_mv < 4500 || adc->vdd_mv > 5500) st |= ST_VDD;                /* MCU / CAN / ADC reference rail */
    if (!bsp_can_healthy()) st |= ST_CAN;
    uint32_t bf = bsp_self_test_faults();
    if (bf & (BSP_FAULT_FLASH_CRC | BSP_FAULT_IMAGE_UNSIGNED)) st |= ST_FLASH;
    if (bf & BSP_FAULT_RAM) st |= ST_RAM;
    if (bf & BSP_FAULT_STACK) st |= ST_STACK;
    if (st & (ST_ADC | ST_VDD | ST_CAN | ST_FLASH | ST_RAM | ST_STACK)) fault = true;
    if (fault) st |= ST_FAULT;
    g_status = st;
    return fault;
}

static uint8_t sat8(uint32_t v) { return (uint8_t)(v > 255u ? 255u : v); }

static void config_load(void)
{
    uint8_t rec[8];
    fuel_cfg_default(&g_cfg);
    if (!bsp_config_read(rec)) g_cfg_src = 0;
    else g_cfg_src = fuel_cfg_decode_record(rec, &g_cfg) ? 1 : 2;
    g_frame.fitted = g_cfg.fitted;
}
/* 0x5A8 from the installer: validate, store in flash, apply. Applied even if the flash write fails (until the next reset). */
static void config_frame(const uint8_t d[8])
{
    fuel_cfg_t c; uint8_t rec[8];
    if (!fuel_cfg_parse_frame(d, &c)) { g_cfg_result = 2; return; }
    fuel_cfg_encode_record(&c, rec);
    g_cfg_result = bsp_config_write(rec) ? 1 : 3;
    if (g_cfg_result == 1) g_cfg_src = 1;
    g_cfg = c; g_frame.fitted = c.fitted; g_cfg_writes++;
}

/* 0x5A7 cycles three pages every 100 ms; byte G = fw version << 4 | page. Layout in docs/protocol.md. */
static void send_diag(const bsp_adc_t *adc, uint32_t now_ms, uint8_t page)
{
    uint8_t d[8] = {0};
    if (page == 2) {
        d[0] = g_cfg.fitted; d[1] = g_cfg_src; d[2] = g_cfg_result; d[3] = g_cfg_writes;
    } else if (page == 0) {
        d[0] = (uint8_t)(adc->p_sig_mv >> 8);   d[1] = (uint8_t)adc->p_sig_mv;
        d[2] = (uint8_t)(adc->vs_sens_mv >> 8); d[3] = (uint8_t)adc->vs_sens_mv;
        d[4] = sat8(adc->vbat_mv / 100u);                                       /* 0.1 V */
        d[5] = (uint8_t)(adc->board_temp_c + 40);
    } else {
        uint32_t up_s = now_ms / 1000u; if (up_s > 65535u) up_s = 65535u;
        d[0] = sat8(bsp_can_tx_errors());
        d[1] = sat8(bsp_can_rx_errors());
        d[2] = g_status;
        d[3] = sat8(bsp_can_reinits());
        d[4] = (uint8_t)(up_s >> 8); d[5] = (uint8_t)up_s;
    }
    d[6] = (uint8_t)((FW_VERSION << 4) | (page & 0x0Fu));
    d[7] = fuel_crc8(d, 8, 7, FRAME_CRC_INIT);
    bsp_can_send(DIAG_ID, d);
}

int app_main(void)
{
    bsp_init();
    flex_init(&g_flex); press_init(&g_press); frame_init(&g_frame);
    config_load();
    uint32_t next_ms = bsp_millis(), diag_ms = next_ms, wd_until = 0;
    uint8_t diag_page = 0;
    for (;;) {
#ifdef SIM_MAX_MS
        if (bsp_millis() > SIM_MAX_MS) return 0;
#endif
        uint32_t now = bsp_millis();
        if ((int32_t)(now - next_ms) < 0) continue;      /* replace with WFI + tick in the target BSP */
        next_ms += FUEL_FRAME_PERIOD_MS;
        uint16_t rx_id; uint8_t rx[8];
        while (bsp_can_recv(&rx_id, rx)) if (rx_id == CFG_FRAME_ID) config_frame(rx);
        bsp_adc_t adc;
        bool adc_ok = bsp_adc_read(&adc);
        adc.adc_ok = adc_ok;
        /* 5V_SENS is a +/-5 mV tracking copy of VDD behind 1 % dividers; comparing the two cancels the bandgap tolerance */
        uint16_t tol = adc.vdd_mv / 20u;
        bool vs_tracks = adc_ok && adc.vs_sens_mv + tol >= adc.vdd_mv && adc.vs_sens_mv <= adc.vdd_mv + tol;
        press_update(&g_press, adc_ok ? adc.p_sig_mv : 0, vs_tracks ? adc.vs_sens_mv : 0);
        flex_update(&g_flex, bsp_micros());
        bool fault = module_self_test(&adc, now, &wd_until);
        uint8_t frame[8];
        frame_build(&g_frame, now, &g_flex, &g_press, fault, frame);
        bsp_can_send(FUEL_FRAME_ID, frame);              /* own TX mailbox; the diag frame below uses a second one */
        if ((int32_t)(now - diag_ms) >= 0) { diag_ms += 100; send_diag(&adc, now, diag_page); diag_page = (uint8_t)((diag_page + 1u) % 3u); }
        bsp_wdog_kick();   /* only kicked after a complete, successful cycle */
    }
}
