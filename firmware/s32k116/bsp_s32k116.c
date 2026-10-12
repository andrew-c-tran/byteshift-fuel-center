/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
/* Board support for the fuel CAN module on NXP S32K116 LQFP-48 at 5 V.
 * Clocks: FIRC 48 MHz core / 24 MHz bus; SOSC 8 MHz crystal feeds FTM0 (1 MHz timestamps) and FlexCAN (500 kbit/s).
 * Pins: PTE4/PTE5 CAN0, PTD15 FTM0_CH0 (flex), PTC14/15/16 ADC0_SE12/13/14 (P_ADC, VS_DIV, VBAT_DIV), PTA2/3 LPUART0. */
#include "S32K116.h"
#include "S32K116_features.h"
#include "bsp.h"
#include "fw_crc32.h"
#include "fuel_config.h"
#include <string.h>

#define CORE_HZ        48000000u
#define BUS_HZ         24000000u
#define ADC_CH_P       12u
#define ADC_CH_VS      13u
#define ADC_CH_VBAT    14u
#define ADC_CH_TEMP    26u    /* internal temperature sensor */
#define ADC_CH_BG      27u    /* band gap 1.0 V */
#define ADC_CH_VREFSH  29u
#define ADC_CH_VREFSL  30u
#define ADC_CH_OFF     31u
#define VBG_MV         1000u
#define WDOG_TIMEOUT_TICKS 6400u   /* 50 ms at LPO 128 kHz */

/* Cortex-M0+ core registers (s32_core_cm0.h does not provide CMSIS helpers) */
#define NVIC_ISER      (*(volatile uint32_t *)0xE000E100u)
#define NVIC_ICPR      (*(volatile uint32_t *)0xE000E280u)
#define SYST_CSR       (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR       (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR       (*(volatile uint32_t *)0xE000E018u)

static volatile uint32_t g_ms;
static volatile uint32_t g_ftm_hi;       /* FTM0 overflow count: 65.536 ms per overflow */
#define CAN_TX_MBS     2u                /* one TX mailbox per CAN ID: MB0 first ID seen (0x5A6), MB1 second (0x5A7) */
#define CAN_RX_MB      2u                /* RX mailbox, exact match on CFG_FRAME_ID (standard data frames only) */
static uint16_t g_mb_id[CAN_TX_MBS];
static bool     g_mb_used[CAN_TX_MBS];
static uint8_t  g_tx_stall[CAN_TX_MBS];
static uint32_t g_can_reinits;
static bool     g_wdog_reset;
static uint32_t g_faults;                /* BSP_FAULT_* */

/* ------------------------------------------------------------------ boot self-tests: flash CRC, RAM march, stack canary */
#define FLASH_CRC_ADDR  0x0001FFFCu      /* .fw_crc word, see the linker script and tools/sign_image.py */
#define CANARY_WORD     0xC0DE5AFEu
#define CANARY_WORDS    8u
extern uint32_t __VECTOR_TABLE[];        /* flash start (0x00000000) */
extern uint32_t __RAM_START[];           /* 0x20000000 */
extern uint32_t __StackLimit[];          /* lowest stack address; the canary lives here */
const uint32_t fw_crc_word __attribute__((section(".fw_crc"), used)) = 0xFFFFFFFFu;

static uint32_t flash_check(void)
{
    uint32_t stored = *(const volatile uint32_t *)FLASH_CRC_ADDR;
    if (stored == 0xFFFFFFFFu) return BSP_FAULT_IMAGE_UNSIGNED;
    return fw_crc32((const uint8_t *)__VECTOR_TABLE, FLASH_CRC_ADDR) == stored ? 0u : BSP_FAULT_FLASH_CRC;
}
/* Non-destructive 0x55/0xAA march over all SRAM below the live stack frames (each word is restored). Runs with
 * interrupts masked, before any peripheral is started, so nothing else touches RAM meanwhile. */
static uint32_t __attribute__((noinline)) ram_march(void)
{
    uint32_t sp, bad = 0;
    __asm volatile ("mov %0, sp" : "=r" (sp));
    volatile uint32_t *p = (volatile uint32_t *)__RAM_START;
    volatile uint32_t *end = (volatile uint32_t *)(sp - 64u);
    __asm volatile ("cpsid i");
    for (; p < end; p++) {
        uint32_t keep = *p;
        *p = 0x55555555u; if (*p != 0x55555555u) bad++;
        *p = 0xAAAAAAAAu; if (*p != 0xAAAAAAAAu) bad++;
        *p = keep;
    }
    __asm volatile ("cpsie i");
    return bad ? BSP_FAULT_RAM : 0u;
}
static void stack_canary_init(void) { for (uint32_t i = 0; i < CANARY_WORDS; i++) __StackLimit[i] = CANARY_WORD; }
static bool stack_ok(void)
{
    for (uint32_t i = 0; i < CANARY_WORDS; i++) if (__StackLimit[i] != CANARY_WORD) return false;
    return true;
}
uint32_t bsp_self_test_faults(void)
{
    if (!stack_ok()) g_faults |= BSP_FAULT_STACK;
    return g_faults;
}

/* ------------------------------------------------------------------ clocks */
static void clocks_init(void)
{
    /* system oscillator: 8 MHz crystal, low gain, medium range; DIV1 and DIV2 outputs /1 */
    SCG->SOSCDIV = SCG_SOSCDIV_SOSCDIV1(1) | SCG_SOSCDIV_SOSCDIV2(1);
    SCG->SOSCCFG = SCG_SOSCCFG_RANGE(2) | SCG_SOSCCFG_EREFS_MASK;
    while (SCG->SOSCCSR & SCG_SOSCCSR_LK_MASK) {}
    SCG->SOSCCSR = SCG_SOSCCSR_SOSCEN_MASK;
    while (!(SCG->SOSCCSR & SCG_SOSCCSR_SOSCVLD_MASK)) {}
    /* FIRC 48 MHz (default source), dividers /1 for peripherals; core /1, bus /2, flash /2 */
    SCG->FIRCDIV = SCG_FIRCDIV_FIRCDIV1(1) | SCG_FIRCDIV_FIRCDIV2(1);
    SCG->RCCR = SCG_RCCR_SCS(3) | SCG_RCCR_DIVCORE(0) | SCG_RCCR_DIVBUS(1) | SCG_RCCR_DIVSLOW(1);
    while (((SCG->CSR & SCG_CSR_SCS_MASK) >> 24) != 3u) {}
}

static void pins_init(void)
{
    PCC->PCCn[PCC_PORTA_INDEX] |= PCC_PCCn_CGC_MASK;
    PCC->PCCn[PCC_PORTC_INDEX] |= PCC_PCCn_CGC_MASK;
    PCC->PCCn[PCC_PORTD_INDEX] |= PCC_PCCn_CGC_MASK;
    PCC->PCCn[PCC_PORTE_INDEX] |= PCC_PCCn_CGC_MASK;
    PORTE->PCR[4]  = PORT_PCR_MUX(5);   /* CAN0_RX */
    PORTE->PCR[5]  = PORT_PCR_MUX(5);   /* CAN0_TX */
    PORTD->PCR[15] = PORT_PCR_MUX(2);   /* FTM0_CH0 flex input (Schmitt-buffered, no pull) */
    PORTC->PCR[14] = PORT_PCR_MUX(0);   /* ADC0_SE12 pressure */
    PORTC->PCR[15] = PORT_PCR_MUX(0);   /* ADC0_SE13 sensor 5 V / 2 */
    PORTC->PCR[16] = PORT_PCR_MUX(0);   /* ADC0_SE14 battery / 11 */
    PORTA->PCR[2]  = PORT_PCR_MUX(6);   /* LPUART0_RX */
    PORTA->PCR[3]  = PORT_PCR_MUX(6);   /* LPUART0_TX */
}

/* ------------------------------------------------------------------ SysTick 1 ms */
static void systick_init(void)
{
    SYST_RVR = CORE_HZ / 1000u - 1u; SYST_CVR = 0; SYST_CSR = 7u;   /* core clock, interrupt, enable */
}
void SysTick_Handler(void) { g_ms++; }
uint32_t bsp_millis(void) { return g_ms; }

/* ------------------------------------------------------------------ FTM0: 1 MHz free-running, CH0 capture both edges */
static void ftm_init(void)
{
    PCC->PCCn[PCC_FTM0_INDEX] = PCC_PCCn_PCS(1) | PCC_PCCn_CGC_MASK;    /* SOSCDIV1 = 8 MHz */
    FTM0->MODE = FTM_MODE_WPDIS_MASK;
    FTM0->SC = 0; FTM0->CNTIN = 0; FTM0->MOD = 0xFFFFu; FTM0->CNT = 0;
    FTM0->FILTER = FTM_FILTER_CH0FVAL(15);                                /* ~60 us input filter */
    FTM0->CONTROLS[0].CnSC = FTM_CnSC_ELSA_MASK | FTM_CnSC_ELSB_MASK | FTM_CnSC_CHIE_MASK;
    FTM0->SC = FTM_SC_CLKS(1) | FTM_SC_PS(3) | FTM_SC_TOIE_MASK;          /* 8 MHz / 8 = 1 MHz */
    NVIC_ICPR = (1u << FTM0_Ch0_7_IRQn) | (1u << FTM0_Ovf_Reload_IRQn);
    NVIC_ISER = (1u << FTM0_Ch0_7_IRQn) | (1u << FTM0_Ovf_Reload_IRQn);
}
void FTM0_Ovf_Reload_IRQHandler(void)
{
    FTM0->SC &= ~FTM_SC_TOF_MASK;
    g_ftm_hi++;
}
void FTM0_Ch0_7_IRQHandler(void)
{
    if (FTM0->CONTROLS[0].CnSC & FTM_CnSC_CHF_MASK) {
        uint32_t cv = FTM0->CONTROLS[0].CnV & 0xFFFFu;
        FTM0->CONTROLS[0].CnSC &= ~FTM_CnSC_CHF_MASK;
        uint32_t hi = g_ftm_hi;
        if ((FTM0->SC & FTM_SC_TOF_MASK) && cv < 0x8000u) hi++;          /* wrapped before the overflow ISR ran */
        bool level_high = (PTD->PDIR >> 15) & 1u;                          /* pulses are >= 0.5 ms, so the level now is the level after the edge */
        app_flex_edge((hi << 16) | cv, level_high);
    }
}
uint32_t bsp_micros(void)
{
    uint32_t hi, cnt, tof;
    __asm volatile ("cpsid i");
    hi = g_ftm_hi; cnt = FTM0->CNT & 0xFFFFu; tof = FTM0->SC & FTM_SC_TOF_MASK;
    if (tof) { cnt = FTM0->CNT & 0xFFFFu; hi++; }
    __asm volatile ("cpsie i");
    return (hi << 16) | cnt;
}

/* ------------------------------------------------------------------ ADC0: 12-bit, 16x hardware average, bandgap-referenced */
static void adc_init(void)
{
    PCC->PCCn[PCC_ADC0_INDEX] = PCC_PCCn_PCS(3) | PCC_PCCn_CGC_MASK;    /* FIRCDIV2 = 48 MHz */
    ADC0->SC1[0] = ADC_SC1_ADCH(ADC_CH_OFF);
    ADC0->CFG1 = ADC_CFG1_ADIV(2) | ADC_CFG1_MODE(1) | ADC_CFG1_ADICLK(0); /* /4 = 12 MHz, 12-bit */
    ADC0->CFG2 = ADC_CFG2_SMPLTS(63);                                     /* long sample for the 4.7 k / 10 k sources */
    ADC0->SC2 = 0;                                                        /* software trigger, VREFH/VREFL */
    ADC0->SC3 = ADC_SC3_AVGE_MASK | ADC_SC3_AVGS(3) | ADC_SC3_CAL_MASK;   /* 32x average during calibration */
    while (ADC0->SC3 & ADC_SC3_CAL_MASK) {}
    ADC0->SC3 = ADC_SC3_AVGE_MASK | ADC_SC3_AVGS(2);                      /* 16x average in operation */
}
static uint16_t adc_read(uint8_t ch)
{
    ADC0->SC1[0] = ADC_SC1_ADCH(ch);
    while (!(ADC0->SC1[0] & ADC_SC1_COCO_MASK)) {}
    return (uint16_t)ADC0->R[0];
}
bool bsp_adc_read(bsp_adc_t *o)
{
    uint16_t bg = adc_read(ADC_CH_BG), vh = adc_read(ADC_CH_VREFSH), vl = adc_read(ADC_CH_VREFSL);
    uint16_t p = adc_read(ADC_CH_P), vs = adc_read(ADC_CH_VS), vb = adc_read(ADC_CH_VBAT), t = adc_read(ADC_CH_TEMP);
    bool ok = (bg > 600 && bg < 1100 && vh > 4000 && vl < 60);           /* bandgap 1.0 V ~ 819 counts at 5 V ref */
    uint32_t vdd_mv = ok ? (uint32_t)VBG_MV * 4095u / bg : 0;
    o->vdd_mv     = (uint16_t)vdd_mv;
    o->p_sig_mv   = (uint16_t)((uint32_t)p * vdd_mv / 4095u);
    o->vs_sens_mv = (uint16_t)((uint32_t)vs * vdd_mv * 2u / 4095u);      /* 10 k / 10 k divider */
    o->vbat_mv    = (uint16_t)((uint32_t)vb * vdd_mv * 11u / 4095u);     /* 100 k / 10 k divider */
    /* on-chip sensor: T = 25 - (Vtemp - 701.2 mV) / 1.646 mV/C (S32K1xx datasheet typicals; approximate) */
    int32_t vtemp_mv = (int32_t)((uint32_t)t * vdd_mv / 4095u);
    o->board_temp_c = (int16_t)(25 - (vtemp_mv - 701) * 1000 / 1646);
    o->adc_ok = ok;
    return ok;
}

/* ------------------------------------------------------------------ FlexCAN0, 500 kbit/s from the 8 MHz oscillator, MB0 for TX */
static void can_init(void)
{
    PCC->PCCn[PCC_FlexCAN0_INDEX] |= PCC_PCCn_CGC_MASK;
    CAN0->MCR |= CAN_MCR_MDIS_MASK;
    CAN0->CTRL1 &= ~CAN_CTRL1_CLKSRC_MASK;                                /* PE clock = SOSCDIV2 (8 MHz) */
    CAN0->MCR &= ~CAN_MCR_MDIS_MASK;
    while (CAN0->MCR & CAN_MCR_LPMACK_MASK) {}
    CAN0->MCR |= CAN_MCR_FRZ_MASK | CAN_MCR_HALT_MASK;
    while (!(CAN0->MCR & CAN_MCR_FRZACK_MASK)) {}
    /* 16 tq: PROPSEG 7, PSEG1 5, PSEG2 3, RJW 3, sample point 81 % */
    CAN0->CTRL1 = CAN_CTRL1_PRESDIV(0) | CAN_CTRL1_PROPSEG(6) | CAN_CTRL1_PSEG1(4) | CAN_CTRL1_PSEG2(2) | CAN_CTRL1_RJW(2);
    for (uint32_t i = 0; i < CAN_RAMn_COUNT; i++) CAN0->RAMn[i] = 0;
    for (uint32_t i = 0; i < CAN_RXIMR_COUNT; i++) CAN0->RXIMR[i] = 0xFFFFFFFFu;
    CAN0->RXMGMASK = 0xFFFFFFFFu;
    CAN0->RAMn[CAN_RX_MB * 4u + 1u] = ((uint32_t)CFG_FRAME_ID & 0x7FFu) << 18;   /* ID, IDE=0, RTR=0; RXIMR all ones = exact */
    CAN0->RAMn[CAN_RX_MB * 4u]      = 0x4u << 24;                                  /* CODE = RX EMPTY */
    CAN0->MCR = (CAN0->MCR & ~(CAN_MCR_FRZ_MASK | CAN_MCR_HALT_MASK | CAN_MCR_MAXMB_MASK)) | CAN_MCR_MAXMB(15) | CAN_MCR_SRXDIS_MASK | CAN_MCR_IRMQ_MASK;
    while (CAN0->MCR & CAN_MCR_FRZACK_MASK) {}
    while (CAN0->MCR & CAN_MCR_NOTRDY_MASK) {}
    for (uint32_t i = 0; i < CAN_TX_MBS; i++) g_tx_stall[i] = 0;
}
/* Each CAN ID gets its own mailbox, so the 0x5A7 diag frame no longer collides with the 0x5A6 frame sent a few
 * microseconds earlier. A mailbox still pending when its ID comes round again (10 ms for 0x5A6, 100 ms for 0x5A7)
 * counts as a stall; 5 stalls restart the controller. */
bool bsp_can_send(uint16_t id, const uint8_t d[8])
{
    uint32_t mb = CAN_TX_MBS;
    for (uint32_t i = 0; i < CAN_TX_MBS; i++) {
        if (g_mb_used[i] && g_mb_id[i] == id) { mb = i; break; }
        if (!g_mb_used[i] && mb == CAN_TX_MBS) mb = i;
    }
    if (mb == CAN_TX_MBS) return false;                                   /* more IDs than mailboxes: not a design case */
    g_mb_used[mb] = true; g_mb_id[mb] = id;
    volatile uint32_t *m = &CAN0->RAMn[mb * 4u];
    if (((m[0] >> 24) & 0xFu) == 0xCu) {                                  /* previous frame still waiting: bus problem */
        if (++g_tx_stall[mb] >= 5u) { can_init(); g_can_reinits++; }
        return false;
    }
    g_tx_stall[mb] = 0;
    CAN0->IFLAG1 = 1u << mb;                                              /* clear this mailbox's flag */
    m[2] = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    m[3] = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
    m[1] = ((uint32_t)id & 0x7FFu) << 18;
    m[0] = (0xCu << 24) | (8u << 16);                                     /* CODE=TX DATA, standard ID, DLC 8 */
    return true;
}
bool bsp_can_healthy(void)
{
    uint32_t fltconf = (CAN0->ESR1 & CAN_ESR1_FLTCONF_MASK) >> 4;         /* 0 active, 1 passive, 2/3 bus off */
    if (fltconf >= 2u) return false;
    for (uint32_t i = 0; i < CAN_TX_MBS; i++) if (g_tx_stall[i]) return false;
    return true;
}
bool bsp_can_recv(uint16_t *id, uint8_t d[8])
{
    if (!(CAN0->IFLAG1 & (1u << CAN_RX_MB))) return false;
    volatile uint32_t *m = &CAN0->RAMn[CAN_RX_MB * 4u];
    uint32_t cs = m[0];                                                   /* reading CS locks the mailbox */
    uint32_t idw = m[1], w0 = m[2], w1 = m[3];
    (void)CAN0->TIMER;                                                    /* unlock */
    CAN0->IFLAG1 = 1u << CAN_RX_MB;
    m[0] = 0x4u << 24;                                                    /* re-arm: RX EMPTY */
    uint32_t code = (cs >> 24) & 0xFu;
    if ((code != 0x2u && code != 0x6u) || ((cs >> 16) & 0xFu) != 8u) return false;   /* not FULL/OVERRUN, or DLC != 8 */
    *id = (uint16_t)((idw >> 18) & 0x7FFu);
    d[0] = (uint8_t)(w0 >> 24); d[1] = (uint8_t)(w0 >> 16); d[2] = (uint8_t)(w0 >> 8); d[3] = (uint8_t)w0;
    d[4] = (uint8_t)(w1 >> 24); d[5] = (uint8_t)(w1 >> 16); d[6] = (uint8_t)(w1 >> 8); d[7] = (uint8_t)w1;
    return true;
}
uint32_t bsp_can_tx_errors(void) { return CAN0->ECR & CAN_ECR_TXERRCNT_MASK; }
uint32_t bsp_can_rx_errors(void) { return (CAN0->ECR >> 8) & 0xFFu; }
uint32_t bsp_can_reinits(void)   { return g_can_reinits; }

/* ------------------------------------------------------------------ installer config in FlexNVM data flash (first phrase)
 * The FlexNVM is a separate flash block from the P-flash the code runs from, so no RAM-resident routine and no interrupt
 * masking is needed (read-while-write between blocks is supported); the capture ISR keeps running during the erase.
 * FCCOB byte order in the NXP header: FCCOB[3..0] = FCCOB0..3 (command, address 23:0), FCCOB[4..11] = data bytes 0..7
 * (FCCOB7 holds byte 0). Data flash is addressed as 0x80_0000 + offset in commands. */
#define CFG_FLASH_ADDR    FEATURE_FLS_DF_START_ADDRESS    /* 0x10000000, memory-mapped read */
#define CFG_FLASH_CMDADDR 0x800000u                       /* same phrase in the FTFC command address space */
#define FTFC_CMD_PROGRAM_PHRASE 0x07u
#define FTFC_CMD_ERASE_SECTOR   0x09u
static bool ftfc_run(uint8_t cmd)
{
    while (!(FTFC->FSTAT & FTFC_FSTAT_CCIF_MASK)) {}
    FTFC->FSTAT = FTFC_FSTAT_ACCERR_MASK | FTFC_FSTAT_FPVIOL_MASK | FTFC_FSTAT_RDCOLERR_MASK;   /* w1c */
    FTFC->FCCOB[3] = cmd;
    FTFC->FCCOB[2] = (uint8_t)(CFG_FLASH_CMDADDR >> 16); FTFC->FCCOB[1] = (uint8_t)(CFG_FLASH_CMDADDR >> 8); FTFC->FCCOB[0] = (uint8_t)CFG_FLASH_CMDADDR;
    FTFC->FSTAT = FTFC_FSTAT_CCIF_MASK;                                   /* launch */
    while (!(FTFC->FSTAT & FTFC_FSTAT_CCIF_MASK)) bsp_wdog_kick();        /* sector erase can exceed the 50 ms watchdog */
    return (FTFC->FSTAT & (FTFC_FSTAT_ACCERR_MASK | FTFC_FSTAT_FPVIOL_MASK | FTFC_FSTAT_MGSTAT0_MASK)) == 0u;
}
bool bsp_config_read(uint8_t rec[8])
{
    const volatile uint8_t *f = (const volatile uint8_t *)CFG_FLASH_ADDR;
    bool blank = true;
    for (uint32_t i = 0; i < 8u; i++) { rec[i] = f[i]; if (rec[i] != 0xFFu) blank = false; }
    return !blank;
}
bool bsp_config_write(const uint8_t rec[8])
{
    uint32_t ocm0 = MSCM->OCMDR[0], ocm1 = MSCM->OCMDR[1];
    MSCM->OCMDR[0] |= MSCM_OCMDR_OCM1(3); MSCM->OCMDR[1] |= MSCM_OCMDR_OCM1(3);   /* flash caches off while writing */
    bool ok = ftfc_run(FTFC_CMD_ERASE_SECTOR);
    if (ok) {
        for (uint32_t i = 0; i < 8u; i++) FTFC->FCCOB[4u + i] = rec[i];
        ok = ftfc_run(FTFC_CMD_PROGRAM_PHRASE);
    }
    MSCM->OCMDR[0] = ocm0; MSCM->OCMDR[1] = ocm1;
    uint8_t back[8];
    return ok && bsp_config_read(back) && memcmp(back, rec, 8) == 0;
}

/* ------------------------------------------------------------------ watchdog: LPO 128 kHz, 50 ms, kicked only after a full cycle */
static void wdog_init(void)
{
    __asm volatile ("cpsid i");
    WDOG->CNT = FEATURE_WDOG_UNLOCK_VALUE;
    while (!(WDOG->CS & WDOG_CS_ULK_MASK)) {}
    WDOG->TOVAL = WDOG_TIMEOUT_TICKS;
    WDOG->WIN = 0;
    WDOG->CS = WDOG_CS_CMD32EN_MASK | WDOG_CS_CLK(1) | WDOG_CS_EN_MASK | WDOG_CS_UPDATE_MASK;
    __asm volatile ("cpsie i");
    while (!(WDOG->CS & WDOG_CS_RCS_MASK)) {}
}
void bsp_wdog_kick(void) { WDOG->CNT = FEATURE_WDOG_TRIGGER_VALUE; }
bool bsp_reset_was_watchdog(void) { return g_wdog_reset; }

/* ------------------------------------------------------------------ LPUART0 115200 8N1 debug console (TX only) */
static void uart_init(void)
{
    PCC->PCCn[PCC_LPUART0_INDEX] = PCC_PCCn_PCS(3) | PCC_PCCn_CGC_MASK;  /* FIRCDIV2 48 MHz */
    LPUART0->BAUD = LPUART_BAUD_OSR(15) | LPUART_BAUD_SBR(26);           /* 48e6 / (16 * 26) = 115385 */
    LPUART0->CTRL = LPUART_CTRL_TE_MASK;
}
void bsp_debug_puts(const char *s)
{
    while (*s) { while (!(LPUART0->STAT & LPUART_STAT_TDRE_MASK)) {} LPUART0->DATA = (uint8_t)*s++; }
}

/* ------------------------------------------------------------------ */
void bsp_init(void)
{
    g_wdog_reset = (RCM->SRS & RCM_SRS_WDOG_MASK) != 0;                  /* SystemInit already disabled the boot WDOG */
    g_faults = ram_march();                                               /* before anything else owns RAM */
    stack_canary_init();
    g_faults |= flash_check();                                            /* ~50 ms at 48 MHz, before the 50 ms WDOG starts */
    clocks_init();
    pins_init();
    systick_init();
    ftm_init();
    adc_init();
    can_init();
    uart_init();
    wdog_init();
    bsp_debug_puts("fuelcan boot\r\n");
}
