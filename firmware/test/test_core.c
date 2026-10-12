/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ByteShift Labs LLC */
/* Host unit tests for the fuel CAN module core. Build: make -C firmware test */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "fuel_crc8.h"
#include "fw_crc32.h"
#include "flex_decode.h"
#include "pressure.h"
#include "fuel_frame.h"
#include "fuel_config.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, tol) (fabsf((float)(a) - (float)(b)) <= (tol))

/* drive the decoder with an ideal sensor waveform: freq_hz, low pulse pw_us, for n periods, starting at t */
static uint32_t run_flex(flex_t *f, uint32_t t, float freq_hz, uint32_t pw_us, int n)
{
    uint32_t period = (uint32_t)(1.0e6f / freq_hz + 0.5f);
    for (int i = 0; i < n; i++) { flex_edge(f, t, false); flex_edge(f, t + pw_us, true); t += period; }
    return t;
}

static void test_crc(void)
{
    uint8_t v[8] = {0x12,0x34,0x56,0x78,0x9A,0xBC,0xDE,0x00};
    CHECK(fuel_crc8(v, 8, 7, 0xFF) == 0x4E, "CRC-8 test vector");
    uint8_t ex[8] = {0x55,0x82,0x01,0xDB,0x20,0x00,0x00,0x00};
    CHECK(fuel_crc8(ex, 8, 7, 0xFF) == 0x72, "v1 example frame with CRC8");
    const uint8_t s[] = "123456789";
    CHECK(fuel_crc8(s, 9, 0xFF, 0xFF) == 0xB4, "check string (no xorout)");
    CHECK(fw_crc32(s, 9) == 0xCBF43926u, "CRC-32 check string (matches zlib.crc32 in tools/sign_image.py)");
    CHECK(fw_crc32(s, 0) == 0u, "CRC-32 of nothing");
}

static void test_flex(void)
{
    flex_t f; flex_init(&f);
    flex_update(&f, 0);
    CHECK(f.status == FLEX_NO_SIGNAL, "no edges -> no signal");

    /* 135 Hz, 2450 us low = 85 %, 20 C */
    uint32_t t = run_flex(&f, 1000, 135.0f, 2450, 8);
    flex_update(&f, t);
    CHECK(f.status == FLEX_OK, "135 Hz ok, got %d", f.status);
    CHECK(NEAR(f.ethanol_pct, 85.0f, 0.3f), "ethanol %.2f", f.ethanol_pct);
    CHECK(f.temp_ok && NEAR(f.fuel_temp_c, 19.8f, 0.5f), "temp %.2f", f.fuel_temp_c);

    /* single glitch period must be rejected by the median */
    flex_edge(&f, t, false); flex_edge(&f, t + 10, true); flex_edge(&f, t + 20, false); flex_edge(&f, t + 2470, true);
    t = run_flex(&f, t + 7407, 135.0f, 2450, 1);
    flex_update(&f, t);
    CHECK(f.status == FLEX_OK && NEAR(f.ethanol_pct, 85.0f, 0.5f), "glitch filtered, eth %.2f st %d", f.ethanol_pct, f.status);

    /* contaminated: 180 Hz */
    t = run_flex(&f, t, 180.0f, 2450, 8); flex_update(&f, t);
    CHECK(f.status == FLEX_CONTAMINATED, "180 Hz -> contaminated, got %d", f.status);
    CHECK(f.ethanol_pct == 0.0f, "contaminated ethanol forced to 0");

    /* 40 Hz -> low fault */
    t = run_flex(&f, t, 40.0f, 2450, 8); flex_update(&f, t);
    CHECK(f.status == FLEX_FREQ_LOW, "40 Hz low fault");

    /* pulse width out of range: 6 ms at 100 Hz */
    t = run_flex(&f, t, 100.0f, 6000, 8); flex_update(&f, t);
    CHECK(f.status == FLEX_OK && !f.temp_ok, "PW 6 ms -> temp invalid, ethanol still valid");

    /* signal loss: 150 ms with no edges */
    flex_update(&f, t + 150000);
    CHECK(f.status == FLEX_NO_SIGNAL, "timeout -> no signal");
    /* recovery needs 5 fresh periods */
    t = run_flex(&f, t + 150000, 100.0f, 2450, 3); flex_update(&f, t);
    CHECK(f.status == FLEX_NO_SIGNAL, "3 periods not enough after loss");
    t = run_flex(&f, t, 100.0f, 2450, 3); flex_update(&f, t);
    CHECK(f.status == FLEX_OK && NEAR(f.ethanol_pct, 50.0f, 0.3f), "recovered 50 %%");

    /* 32-bit timer wrap */
    flex_t g; flex_init(&g);
    t = run_flex(&g, 0xFFFFFFFFu - 30000, 100.0f, 2450, 8); flex_update(&g, t);
    CHECK(g.status == FLEX_OK && NEAR(g.ethanol_pct, 50.0f, 0.3f), "timer wrap ok");

    /* Missing rising edges must not leave an old valid pulse width attached to fresh falling periods. */
    flex_t missing; flex_init(&missing);
    uint32_t tm = run_flex(&missing, 1000, 100.0f, 2450, 8);
    flex_update(&missing, tm);
    CHECK(missing.status == FLEX_OK && missing.temp_ok, "fixture has valid periods and widths");
    for (int i = 0; i < 6; i++) { flex_edge(&missing, tm, false); tm += 10000; }
    flex_update(&missing, tm);
    CHECK(missing.status != FLEX_OK && !missing.temp_ok,
          "missing rising edges must invalidate flex reading (status %d, temp_ok %d)",
          missing.status, missing.temp_ok);
}

static void test_pressure(void)
{
    press_t p; press_init(&p);
    for (int i = 0; i < 4; i++) press_update(&p, 500, 5000);
    CHECK(p.status == PRESS_NOT_READY, "not ready before 5 samples");
    press_update(&p, 500, 5000);
    CHECK(p.status == PRESS_OK && NEAR(p.kpa, 0.0f, 0.5f), "0.5 V -> 0 kPa (%.1f)", p.kpa);
    for (int i = 0; i < 5; i++) press_update(&p, 4500, 5000);
    CHECK(p.status == PRESS_OK && NEAR(p.kpa, 1034.2f, 1.0f), "4.5 V -> 1034 kPa (%.1f)", p.kpa);
    CHECK(p.implausible, "1034 kPa flagged implausible");
    press_t q; press_init(&q);
    for (int i = 0; i < 5; i++) press_update(&q, 2337, 5000);
    CHECK(q.status == PRESS_OK && NEAR(q.kpa, 475.0f, 1.0f), "2.337 V -> 475 kPa (%.1f)", q.kpa);
    /* ratiometric: same fraction at Vs = 4.8 V */
    for (int i = 0; i < 5; i++) press_update(&q, 2243, 4800);
    CHECK(q.status == PRESS_OK && NEAR(q.kpa, 475.0f, 1.5f), "ratiometric at 4.8 V (%.1f)", q.kpa);
    for (int i = 0; i < 5; i++) press_update(&q, 125, 5000);
    CHECK(q.status == PRESS_SIG_LOW, "sensor under-pressure rail 0.125 V -> fault");
    for (int i = 0; i < 5; i++) press_update(&q, 0, 5000);
    CHECK(q.status == PRESS_SIG_LOW, "open wire with pull-down -> fault");
    for (int i = 0; i < 5; i++) press_update(&q, 4875, 5000);
    CHECK(q.status == PRESS_SIG_HIGH, "sensor fault rail 4.875 V -> fault");
    for (int i = 0; i < 5; i++) press_update(&q, 2337, 4600);
    CHECK(q.status == PRESS_OK && NEAR(q.kpa, 1034.2f * (2337.0f / 4600.0f - 0.1f) / 0.8f, 1.0f), "4.6 V rail inside the wide window, ratiometric");
    for (int i = 0; i < 5; i++) press_update(&q, 2337, 4400);
    CHECK(q.status == PRESS_VS_BAD, "5 V rail sag below 4.5 V -> fault");
    /* fast downward step (pump death) must pass; fast upward step is rejected once */
    for (int i = 0; i < 5; i++) press_update(&q, 2337, 5000);
    for (int i = 0; i < 5; i++) press_update(&q, 500, 5000);
    CHECK(q.status == PRESS_OK && NEAR(q.kpa, 0.0f, 1.0f), "collapse to 0 passes (%d)", q.status);
    for (int i = 0; i < 5; i++) press_update(&q, 4000, 5000);
    CHECK(q.status == PRESS_STEP || q.status == PRESS_OK, "upward jump handled");
}

static void test_frame(void)
{
    flex_t f; flex_init(&f); press_t p; press_init(&p); frame_state_t s; frame_init(&s);
    uint8_t o[8];

    /* startup: everything not ready */
    frame_build(&s, 10, &f, &p, false, o);
    CHECK(o[4] & ST_NOT_READY, "not ready at startup");
    CHECK(o[0] == 0 && o[2] == 0 && o[3] == 0, "safe values at startup");
    CHECK(fuel_crc8(o, 8, 7, 0xFF) == o[7], "crc ok");
    CHECK((o[5] & 0x0F) == 2, "protocol version 2");

    /* healthy: 85 %, 20 C, 475 kPa at t = 2 s */
    uint32_t t = run_flex(&f, 0, 135.0f, 2450, 8); flex_update(&f, t);
    for (int i = 0; i < 5; i++) press_update(&p, 2337, 5000);
    uint32_t now = 2000;
    /* seeding: first stable second -> creep at 3 %/s, then jump to the stable 85 % */
    frame_build(&s, now, &f, &p, false, o);
    CHECK(o[0] == 0 && (o[4] & ST_ETH_SLEW), "ethanol starts at 0 after power-up (got %d)", o[0]);
    for (int i = 0; i < 50; i++) { now += 10; frame_build(&s, now, &f, &p, false, o); }
    CHECK(o[0] <= 2, "still creeping at 0.5 s (got %d)", o[0]);
    for (int i = 0; i < 60; i++) { now += 10; frame_build(&s, now, &f, &p, false, o); }
    CHECK(o[0] == 85 && !(o[4] & ST_ETH_SLEW), "seeded to 85 %% after 1 s stable (got %d st %02X)", o[0], o[4]);
    /* slew: a rise from 85 to 100 during operation is limited to 3 %/s */
    flex_t fhi; flex_init(&fhi); t = run_flex(&fhi, 0, 150.0f, 2450, 8); flex_update(&fhi, t);
    now += 10; frame_build(&s, now, &fhi, &p, false, o);
    CHECK(o[0] == 85 && (o[4] & ST_ETH_SLEW), "upward step slew-limited (got %d st %02X)", o[0], o[4]);
    for (int i = 0; i < 400; i++) { now += 10; frame_build(&s, now, &fhi, &p, false, o); }
    CHECK(NEAR(o[0], 97, 1) && (o[4] & ST_ETH_SLEW), "still ramping at 4 s (got %d)", o[0]);
    for (int i = 0; i < 120; i++) { now += 10; frame_build(&s, now, &fhi, &p, false, o); }
    CHECK(o[0] == 100 && !(o[4] & ST_ETH_SLEW), "reaches 100 %% (got %d)", o[0]);
    for (int i = 0; i < 10; i++) { now += 10; frame_build(&s, now, &f, &p, false, o); }
    CHECK(o[0] == 85, "back down to 85 immediately (got %d)", o[0]);
    CHECK(o[1] == 60, "temp 20 C -> 60 (got %d)", o[1]);
    CHECK(((o[2] << 8) | o[3]) == 475, "475 kPa");
    CHECK(o[4] == 0, "no status bits (got %02X)", o[4]);
    uint8_t c0 = o[5] >> 4; now += 10; frame_build(&s, now, &f, &p, false, o);
    CHECK((o[5] >> 4) == ((c0 + 1) & 0x0F), "rolling counter increments");
    CHECK(o[6] == 135, "raw freq byte");

    /* downward change is immediate: sensor drops to 10 % */
    flex_t f2; flex_init(&f2); t = run_flex(&f2, 0, 60.0f, 2450, 8); flex_update(&f2, t);
    now += 10; frame_build(&s, now, &f2, &p, false, o);
    CHECK(o[0] == 10, "downward step immediate (got %d)", o[0]);

    /* contaminated fuel: ethanol 0 + error + contam bits, sticky for 1 s after recovery */
    flex_t f3; flex_init(&f3); t = run_flex(&f3, 0, 180.0f, 2450, 8); flex_update(&f3, t);
    now += 10; frame_build(&s, now, &f3, &p, false, o);
    CHECK(o[0] == 0 && (o[4] & ST_ETH_ERR) && (o[4] & ST_ETH_CONTAM), "contaminated -> 0 %% + flags (%02X)", o[4]);
    uint32_t t_rec = now;
    for (int i = 0; i < 50; i++) { now += 10; frame_build(&s, now, &f2, &p, false, o); }
    CHECK((o[4] & ST_ETH_ERR) && o[0] == 0, "error sticky 0.5 s after recovery");
    while (now < t_rec + 1100) { now += 10; frame_build(&s, now, &f2, &p, false, o); }
    CHECK(!(o[4] & ST_ETH_ERR), "error cleared after 1 s");
    CHECK(o[0] == 10, "re-seeded from the stable 10 %% reading after the fault (got %d)", o[0]);

    /* pressure fault -> 0 kPa + bit 5 */
    press_t p2; press_init(&p2); for (int i = 0; i < 5; i++) press_update(&p2, 4875, 5000);
    now += 10; frame_build(&s, now, &f2, &p2, false, o);
    CHECK(((o[2] << 8) | o[3]) == 0 && (o[4] & ST_PRESS_ERR), "pressure fault -> 0 kPa + flag");
    /* temp fault -> 125 C + bit 6 */
    flex_t f4; flex_init(&f4); t = run_flex(&f4, 0, 60.0f, 6000, 8); flex_update(&f4, t);
    now += 10; frame_build(&s, now, &f4, &p, false, o);
    CHECK(o[1] == 165 && (o[4] & ST_TEMP_ERR), "temp fault -> 125 C + flag (got %d)", o[1]);
    now += 10; frame_build(&s, now, &f2, &p, true, o);
    CHECK(o[4] & ST_MODULE_FAULT, "module fault bit");
    CHECK(o[0] == 0 && o[1] == 165 && o[2] == 0 && o[3] == 0, "module fault -> fail-safe values (%02X %02X %02X %02X)", o[0], o[1], o[2], o[3]);
    CHECK(!(o[4] & ST_ETH_ERR) && (o[4] & ST_PRESS_ERR), "module fault leaves the sensor bits as measured (eth ok, pressure still sticky)");
    now += 10; frame_build(&s, now, &f2, &p, false, o);
    CHECK(o[0] == 10 && !(o[4] & ST_MODULE_FAULT), "values return as soon as the module fault clears (got %d)", o[0]);
}

static void test_not_fitted(void)
{
    /* flex not fitted, pressure fitted: flex channel reads fail-safe with NO error bits, pressure works normally */
    flex_t none; flex_init(&none); flex_update(&none, 0);                   /* no pulses at all (open input) */
    press_t p; press_init(&p); for (int i = 0; i < 5; i++) press_update(&p, 2337, 5000);
    frame_state_t s; frame_init(&s); s.fitted = CFG_FIT_PRESS; uint8_t o[8]; uint32_t now = 0;
    for (int i = 0; i < 100; i++) { now += 10; frame_build(&s, now, &none, &p, false, o); }
    CHECK(o[0] == 0 && o[1] == 165 && o[6] == 0, "flex not fitted -> 0 %%, 125 C, 0 Hz (got %d %d %d)", o[0], o[1], o[6]);
    CHECK(o[4] == 0, "flex not fitted -> no status bits (got %02X)", o[4]);
    CHECK(((o[2] << 8) | o[3]) == 475, "pressure still reported (got %d)", (o[2] << 8) | o[3]);
    /* a flex sensor that IS fitted but reads nothing must still flag */
    s.fitted = CFG_FIT_ALL;
    for (int i = 0; i < 100; i++) { now += 10; frame_build(&s, now, &none, &p, false, o); }
    CHECK((o[4] & ST_ETH_ERR) && (o[4] & ST_TEMP_ERR), "same input with flex fitted -> error bits (got %02X)", o[4]);
    /* pressure not fitted, flex fitted: open pressure input gives 0 kPa with no bit 5; ethanol normal */
    flex_t e85; flex_init(&e85); uint32_t t = run_flex(&e85, 0, 135.0f, 2450, 8); flex_update(&e85, t);
    press_t open; press_init(&open); for (int i = 0; i < 5; i++) press_update(&open, 0, 5000);
    frame_state_t s2; frame_init(&s2); s2.fitted = CFG_FIT_FLEX;
    for (int i = 0; i < 200; i++) { now += 10; frame_build(&s2, now, &e85, &open, false, o); }
    CHECK(o[0] == 85 && ((o[2] << 8) | o[3]) == 0 && !(o[4] & ST_PRESS_ERR), "pressure not fitted -> 0 kPa, no bit 5, ethanol 85 (got %d st %02X)", o[0], o[4]);
    /* module fault still overrides a fitted channel */
    now += 10; frame_build(&s2, now, &e85, &open, true, o);
    CHECK(o[0] == 0 && (o[4] & ST_MODULE_FAULT), "module fault with not-fitted pressure -> ethanol 0 too");
}

static void test_config(void)
{
    fuel_cfg_t c; uint8_t rec[8], f[8];
    fuel_cfg_default(&c); CHECK(c.fitted == CFG_FIT_ALL, "default = both fitted");
    c.fitted = CFG_FIT_FLEX; fuel_cfg_encode_record(&c, rec);
    CHECK(rec[0] == 'F' && rec[1] == 'C' && rec[2] == CFG_REC_VERSION && rec[3] == 1 && rec[4] == 0xFE, "record layout");
    fuel_cfg_t d = {0xFF}; CHECK(fuel_cfg_decode_record(rec, &d) && d.fitted == CFG_FIT_FLEX, "record round trip");
    memset(f, 0xFF, 8); d.fitted = 0x55; CHECK(!fuel_cfg_decode_record(f, &d) && d.fitted == 0x55, "blank flash rejected, config untouched");
    memcpy(f, rec, 8); f[3] = CFG_FIT_PRESS; CHECK(!fuel_cfg_decode_record(f, &d), "bit flip in the mask rejected (inverse + CRC)");
    memcpy(f, rec, 8); f[7] ^= 1; CHECK(!fuel_cfg_decode_record(f, &d), "bad record CRC rejected");
    /* CAN frame */
    memset(f, 0, 8); f[0] = CFG_FRAME_MAGIC; f[1] = CFG_FIT_PRESS; f[2] = (uint8_t)~f[1]; f[7] = fuel_crc8(f, 8, 7, 0xFF);
    CHECK(fuel_cfg_parse_frame(f, &d) && d.fitted == CFG_FIT_PRESS, "valid 0x5A8 frame");
    f[1] = CFG_FIT_ALL | 0x04; f[2] = (uint8_t)~f[1]; f[7] = fuel_crc8(f, 8, 7, 0xFF);
    CHECK(!fuel_cfg_parse_frame(f, &d) && d.fitted == CFG_FIT_PRESS, "reserved bit rejected");
    f[1] = CFG_FIT_ALL; f[2] = (uint8_t)~f[1]; f[5] = 1; f[7] = fuel_crc8(f, 8, 7, 0xFF);
    CHECK(!fuel_cfg_parse_frame(f, &d), "non-zero reserved byte rejected");
    f[5] = 0; f[7] = fuel_crc8(f, 8, 7, 0xFF); f[0] = 0xC4;
    CHECK(!fuel_cfg_parse_frame(f, &d), "wrong magic rejected");
    f[0] = CFG_FRAME_MAGIC; f[7] ^= 0x80;
    CHECK(!fuel_cfg_parse_frame(f, &d), "wrong CRC rejected");
    f[7] ^= 0x80; CHECK(fuel_cfg_parse_frame(f, &d) && d.fitted == CFG_FIT_ALL, "both fitted accepted");
}

static void test_keyon_drainback(void)
{
    /* sensor reads air (50 Hz = 0 %) for 2 s after key-on, then the pump prime fills it with E85 (135 Hz) */
    flex_t air; flex_init(&air); uint32_t t = run_flex(&air, 0, 50.0f, 2450, 8); flex_update(&air, t);
    flex_t e85; flex_init(&e85); t = run_flex(&e85, 0, 135.0f, 2450, 8); flex_update(&e85, t);
    press_t p; press_init(&p); for (int i = 0; i < 5; i++) press_update(&p, 2337, 5000);
    frame_state_t s; frame_init(&s); uint8_t o[8]; uint32_t now = 0;
    for (int i = 0; i < 200; i++) { now += 10; frame_build(&s, now, &air, &p, false, o); }
    CHECK(o[0] == 0 && o[4] == 0, "air at key-on -> 0 %% seeded, no flags (got %d st %02X)", o[0], o[4]);
    for (int i = 0; i < 200; i++) { now += 10; frame_build(&s, now, &e85, &p, false, o); }
    CHECK(o[0] < 10 && (o[4] & ST_ETH_SLEW), "2 s after prime: still slew-limited (got %d)", o[0]);
    for (int i = 0; i < 310; i++) { now += 10; frame_build(&s, now, &e85, &p, false, o); }
    CHECK(o[0] == 85 && !(o[4] & ST_ETH_SLEW), "re-seeded to 85 %% ~5 s after prime (got %d st %02X)", o[0], o[4]);
    /* a noisy, non-steady rise must NOT re-seed: alternate 85 and 100 every 200 ms for 8 s */
    flex_t e100; flex_init(&e100); t = run_flex(&e100, 0, 150.0f, 2450, 8); flex_update(&e100, t);
    for (int i = 0; i < 800; i++) { now += 10; frame_build(&s, now, (i / 20) % 2 ? &e100 : &e85, &p, false, o); }
    CHECK(o[0] <= 100 && o[0] >= 85, "noisy input stays bounded (got %d)", o[0]);
    /* the ramp is what limits it: 8 s * 3 %/s = 24 > 15, so it may have reached 100 by ramping; verify no jump happened
       earlier than the ramp allows by checking the value 2 s into the noise */
    frame_state_t s2; frame_init(&s2); now = 0;
    for (int i = 0; i < 200; i++) { now += 10; frame_build(&s2, now, &e85, &p, false, o); }
    for (int i = 0; i < 200; i++) { now += 10; frame_build(&s2, now, (i / 20) % 2 ? &e100 : &e85, &p, false, o); }
    CHECK(o[0] <= 92, "noise: no re-seed, only the 3 %%/s ramp (got %d)", o[0]);
}

int main(void)
{
    test_keyon_drainback();
    test_crc(); test_flex(); test_pressure(); test_frame(); test_not_fitted(); test_config();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
