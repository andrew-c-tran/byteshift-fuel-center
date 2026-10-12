/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
#ifndef BSP_H
#define BSP_H
/* Board support interface. Implemented by firmware/s32k116 (target) and firmware/sim (host). */
#include <stdint.h>
#include <stdbool.h>

typedef struct { uint16_t p_sig_mv, vs_sens_mv, vbat_mv, vdd_mv; int16_t board_temp_c; bool adc_ok; } bsp_adc_t;

/* bsp_self_test_faults() bits. Flash/RAM are evaluated once in bsp_init; the stack canary is re-checked on every call
 * and latched. Any bit set means MODULE_FAULT for the life of this boot. */
#define BSP_FAULT_FLASH_CRC       0x01u   /* image CRC-32 mismatch */
#define BSP_FAULT_IMAGE_UNSIGNED  0x02u   /* CRC word blank: an unsigned build (e.g. ELF loaded by a debugger) */
#define BSP_FAULT_RAM             0x04u   /* SRAM march test failed at boot */
#define BSP_FAULT_STACK           0x08u   /* stack canary overwritten */

void     bsp_init(void);                 /* self-tests, clocks, pins, ADC, CAN, timers, watchdog */
uint32_t bsp_millis(void);
uint32_t bsp_micros(void);
void     bsp_wdog_kick(void);
bool     bsp_reset_was_watchdog(void);
uint32_t bsp_self_test_faults(void);     /* BSP_FAULT_* bits, 0 = healthy */
bool     bsp_adc_read(bsp_adc_t *out);   /* averaged, in millivolts, absolute (bandgap-referenced) */
/* One TX mailbox per distinct CAN ID (target: 2). Returns false if that ID's previous frame is still pending,
 * the mailbox pool is exhausted, or the controller was just restarted. */
bool     bsp_can_send(uint16_t id, const uint8_t data[8]);
bool     bsp_can_healthy(void);          /* error-active or error-passive, no stalled mailbox */
uint32_t bsp_can_tx_errors(void);        /* FlexCAN TX error counter */
uint32_t bsp_can_rx_errors(void);        /* FlexCAN RX error counter */
uint32_t bsp_can_reinits(void);          /* controller restarts since boot (stalled mailbox recovery) */
/* Polled receive of one pending frame. Target: a single RX mailbox filtered on CFG_FRAME_ID (0x5A8). */
bool     bsp_can_recv(uint16_t *id, uint8_t data[8]);
/* 8-byte installer configuration record in data flash (FlexNVM, outside the signed P-flash image).
 * read: false if the record is blank (never written). write: erase + program + verify, false on any flash error. */
bool     bsp_config_read(uint8_t rec[8]);
bool     bsp_config_write(const uint8_t rec[8]);
/* The BSP calls this from the flex-input capture ISR with a 1 MHz timestamp. Implemented in app/main.c. */
void     app_flex_edge(uint32_t t_us, bool rising);
void     bsp_debug_puts(const char *s);  /* optional console */
#endif
