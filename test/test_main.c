// Host-side unit tests for the hardware independent modules.
// Build & run: make -C test

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hd_rumble.h"
#include "mapping.h"
#include "s2_proto.h"
#include "settings.h"

static int g_fail, g_pass;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (cond) {                                                                 \
            g_pass++;                                                               \
        } else {                                                                    \
            g_fail++;                                                               \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
        }                                                                           \
    } while (0)

#define CHECK_NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

// settings.c depends on the Pico SDK; provide the pieces the modules need.
settings_t g_settings;
const char *in_button_name(in_button_t b) { (void)b; return "?"; }
const char *out_button_name(out_button_t b) { (void)b; return "?"; }

static void test_defaults(settings_t *s) {
    memset(s, 0, sizeof *s);
    for (int i = IN_A; i <= IN_RIGHT; i++) s->button_map[i] = (uint8_t)(OUT_A + i);
    s->stick_deadzone_pct = 0;
    s->stick_outer_pct = 100;
    s->gyro_enabled = 1;
    s->gyro_scale_pct = 100;
    s->accel_scale_pct = 100;
    s->gc_trigger_threshold = 120;
}

// ---------------------------------------------------------------------------
static void test_s1_rumble_classic(void) {
    s1_rumble_state_t st;
    rumble_sample_t out[3];
    s1_rumble_reset(&st);

    // Neutral frame: 320 Hz / 160 Hz, silent.
    const uint8_t neutral[4] = {0x00, 0x01, 0x40, 0x40};
    int n = s1_rumble_decode(&st, neutral, out);
    CHECK(n == 1);
    CHECK_NEAR(out[0].hi_freq_hz, 320.0, 0.5);
    CHECK_NEAR(out[0].lo_freq_hz, 160.0, 0.5);
    CHECK(out[0].hi_amp == 0.0f && out[0].lo_amp == 0.0f);

    // SDL encoding of max amplitude: high amp byte 0xc8 (code 100), low 0x0072.
    // HF 320 Hz (0x0100) -> bytes 00 01|c8 ; LF 160 Hz (0x40) | (0x72 << 8)
    const uint8_t full[4] = {0x00, 0x01 | 0xc8, 0x40, 0x72};
    n = s1_rumble_decode(&st, full, out);
    CHECK(n == 1);
    CHECK_NEAR(out[0].hi_amp, 1.0, 0.02);
    CHECK_NEAR(out[0].lo_amp, 1.0, 0.02);
    CHECK_NEAR(out[0].hi_freq_hz, 320.0, 0.5);

    // All zero means stop.
    const uint8_t zero[4] = {0, 0, 0, 0};
    s1_rumble_decode(&st, zero, out);
    CHECK(out[0].hi_amp == 0.0f && out[0].lo_amp == 0.0f);

    // Frequency check: HF 1252 Hz is the documented max (hf byte 0xfc,0x01).
    const uint8_t hf_hi[4] = {0xfc, 0x01 | 0xc8, 0x40, 0x40};
    s1_rumble_decode(&st, hf_hi, out);
    CHECK_NEAR(out[0].hi_freq_hz, 1252.6, 2.0);
}

static void test_s1_rumble_packed(void) {
    s1_rumble_state_t st;
    rumble_sample_t out[3];
    s1_rumble_reset(&st);
    // Mode 3: three 5-bit updates per band, preset 1 (= max) then silence then preset 3.
    uint32_t w = (3u << 30) | (1u << 25) | (1u << 20) | (0u << 15) | (0u << 10) | (3u << 5) | 3u;
    uint8_t b[4] = {(uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24)};
    int n = s1_rumble_decode(&st, b, out);
    CHECK(n == 3);
    CHECK_NEAR(out[0].lo_amp, s1_amp_code_to_linear(127), 0.01);
    CHECK(out[1].lo_amp == 0.0f);
    CHECK_NEAR(out[2].lo_amp, s1_amp_code_to_linear(127) / 2.0, 0.01);   // preset 3 = -1.0 log2

    // Mode 0 holds the state.
    const uint8_t hold[4] = {0x01, 0x00, 0x00, 0x00};
    n = s1_rumble_decode(&st, hold, out);
    CHECK(n == 1);
    CHECK_NEAR(out[0].lo_amp, s1_amp_code_to_linear(127) / 2.0, 0.01);
}

static void test_s2_rumble_encode(void) {
    // The console's idle frame is 87 01 20 11 00 (SDL / TommyWabg capture).
    uint8_t f[5];
    s2_rumble_encode_frame(S2_RUMBLE_NEUTRAL_HI_FREQ, 0, S2_RUMBLE_NEUTRAL_LO_FREQ, 0, f);
    const uint8_t idle[5] = {0x87, 0x01, 0x20, 0x11, 0x00};
    CHECK(memcmp(f, idle, 5) == 0);

    s2_rumble_params_t p = {.translate_freq = true, .freq_slope = 117, .strength_pct = 100};
    CHECK(s2_rumble_freq_code(320.0f, true, &p) == 0x187);
    CHECK(s2_rumble_freq_code(160.0f, false, &p) == 0x112);
    p.translate_freq = false;
    CHECK(s2_rumble_freq_code(1000.0f, true, &p) == 0x187);
    CHECK(s2_rumble_amp_code(0.0f, &p) == 0);
    CHECK(s2_rumble_amp_code(1.0f, &p) == 450);
    p.strength_pct = 200;
    CHECK(s2_rumble_amp_code(1.6f, &p) == 1023);

    rumble_sample_t s = {320.0f, 0.5f, 160.0f, 0.0f};
    uint8_t block[S2_RUMBLE_BLOCK_LEN];
    p.strength_pct = 100;
    s2_rumble_encode_block(5, &s, 1, &p, block);
    CHECK(block[0] == 0x55);
    CHECK(s2_rumble_block_active(block));
    // Repeated into all three frames.
    CHECK(memcmp(block + 1, block + 6, 5) == 0 && memcmp(block + 1, block + 11, 5) == 0);
    s2_rumble_encode_block(0, NULL, 0, &p, block);
    CHECK(!s2_rumble_block_active(block));
}

// ---------------------------------------------------------------------------
static void test_adv(void) {
    // Example adverts from ndeadly's research (manufacturer data payload).
    const uint8_t pairing[] = {0x53, 0x05, 0x01, 0x00, 0x03, 0x7e, 0x05, 0x69, 0x20, 0x00, 0x01, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t wake[] = {0x53, 0x05, 0x01, 0x00, 0x03, 0x7e, 0x05, 0x69, 0x20, 0x00, 0x01, 0x81, 0x5f,
                            0x11, 0x85, 0xeb, 0xf1, 0x48, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    s2_adv_info_t a;
    CHECK(s2_parse_manufacturer_data(pairing, sizeof pairing, &a));
    CHECK(a.pid == S2_PID_PRO2 && a.vid == 0x057E && a.pairing_mode && !a.wake_console);
    CHECK(s2_parse_manufacturer_data(wake, sizeof wake, &a));
    CHECK(!a.pairing_mode && a.wake_console && a.host_addr_le[0] == 0x5f && a.host_addr_le[5] == 0x48);
    uint8_t bad[sizeof pairing];
    memcpy(bad, pairing, sizeof bad);
    bad[0] = 0x4c;
    CHECK(!s2_parse_manufacturer_data(bad, sizeof bad, &a));
}

static void test_commands(void) {
    uint8_t buf[64];
    size_t n = s2_build_memory_read(buf, sizeof buf, 0x13000, 0x40);
    const uint8_t expect[] = {0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00,
                              0x40, 0x7e, 0x00, 0x00, 0x00, 0x30, 0x01, 0x00};
    CHECK(n == sizeof expect && memcmp(buf, expect, n) == 0);

    // Response parsing + memory read validation.
    uint8_t rsp[8 + 8 + 4] = {0x02, 0x01, 0x01, 0x04, 0x10, 0x78, 0x00, 0x00,
                              0x04, 0x00, 0x00, 0x00, 0x40, 0x31, 0x01, 0x00, 0xde, 0xad, 0xbe, 0xef};
    s2_response_t r;
    CHECK(s2_parse_response(rsp, sizeof rsp, &r));
    const uint8_t *payload;
    CHECK(s2_parse_memory_read(&r, 0x13140, 4, &payload));
    CHECK(payload[0] == 0xde && payload[3] == 0xef);
    CHECK(!s2_parse_memory_read(&r, 0x13000, 4, &payload));
}

static void test_input_report(void) {
    uint8_t d[63];
    memset(d, 0, sizeof d);
    d[4] = 0x08;                  // A
    d[7] = 0x02;                  // GL (bit 25)
    d[0x0A] = 0x00; d[0x0B] = 0x08; d[0x0C] = 0x80;   // left stick 0x800, 0x800
    d[0x1F] = 0x10; d[0x20] = 0x0F;                   // 3856 mV
    d[0x30] = 0x00; d[0x31] = 0x10;                   // accel x = 4096
    d[0x36] = 0x01;                                   // gyro x = 1
    s2_input_t in;
    CHECK(s2_parse_input_report(d, sizeof d, &in));
    CHECK(in.buttons == (S2_BTN_A | S2_BTN_GL));
    CHECK(in.stick_l[0] == 0x800 && in.stick_l[1] == 0x800);
    CHECK(in.battery_mv == 3856);
    CHECK(in.accel[0] == 4096 && in.gyro[0] == 1);
    CHECK(!s2_parse_input_report(d, 10, &in));
}

// ---------------------------------------------------------------------------
static void test_mapping(void) {
    settings_t s;
    test_defaults(&s);
    s.button_map[IN_GL] = OUT_B;
    mapping_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    s2_default_stick_cal(&ctx.cal_l);
    s2_default_stick_cal(&ctx.cal_r);
    ctx.gyro_lsb_per_dps = S1_GYRO_LSB_PER_DPS;   // unity gyro scale

    s2_input_t in;
    memset(&in, 0, sizeof in);
    in.buttons = S2_BTN_A | S2_BTN_GL | S2_BTN_ZL | S2_BTN_C;
    in.stick_l[0] = 2048 + 1400;  // full right
    in.stick_l[1] = 2048;
    in.stick_r[0] = in.stick_r[1] = 2048;
    in.accel[0] = 100; in.accel[1] = 200; in.accel[2] = 4096;
    in.gyro[0] = 10; in.gyro[1] = 20; in.gyro[2] = 30;

    procon_input_t out;
    mapping_apply(&s, &ctx, &in, &out);
    CHECK(out.buttons == (S1_BTN_A | S1_BTN_B | S1_BTN_ZL));   // C unmapped
    CHECK(out.stick_l[0] == S1_STICK_CENTER + S1_STICK_RANGE);
    CHECK(out.stick_l[1] == S1_STICK_CENTER);
    CHECK(out.stick_r[0] == S1_STICK_CENTER);
    // Switch 1 = (S2.y, -S2.x, S2.z)
    CHECK(out.accel[0] == 200 && out.accel[1] == -100 && out.accel[2] == 4096);
    CHECK(out.gyro[0] == 20 && out.gyro[1] == -10 && out.gyro[2] == 30);

    // Deadzone
    s.stick_deadzone_pct = 10;
    in.stick_l[0] = 2048 + 100;   // ~7% deflection
    mapping_apply(&s, &ctx, &in, &out);
    CHECK(out.stick_l[0] == S1_STICK_CENTER);

    // Packing matches the Switch format.
    uint16_t v[2] = {0x123, 0x456};
    uint8_t p[3];
    mapping_pack_stick(v, p);
    CHECK(p[0] == 0x23 && p[1] == 0x61 && p[2] == 0x45);

    // GameCube analog trigger past the threshold presses L.
    ctx.is_gamecube = true;
    ctx.gc_trigger_neutral[0] = 30;
    memset(&in, 0, sizeof in);
    in.trigger_l = 200;
    CHECK(mapping_buttons(&s, &ctx, &in) & S1_BTN_L);
}

static void test_quick_remap(void) {
    settings_t s;
    test_defaults(&s);
    s.button_map[IN_GL] = OUT_LSTICK;
    in_button_t changed = IN_COUNT;
    uint32_t chord = S2_BTN_C | S2_BTN_GL;

    // C alone, or C + GL without another press: nothing.
    CHECK(!mapping_quick_remap(&s, 0, S2_BTN_C, &changed));
    CHECK(!mapping_quick_remap(&s, S2_BTN_C, chord, &changed));
    CHECK(mapping_quick_remap_held(chord) && !mapping_quick_remap_held(S2_BTN_GL));

    // C + GL + A: GL now sends A.
    CHECK(mapping_quick_remap(&s, chord, chord | S2_BTN_A, &changed));
    CHECK(changed == IN_GL && s.button_map[IN_GL] == OUT_A);
    // Holding A doesn't repeat.
    CHECK(!mapping_quick_remap(&s, chord | S2_BTN_A, chord | S2_BTN_A, &changed));
    // Same again clears it.
    CHECK(mapping_quick_remap(&s, chord, chord | S2_BTN_A, &changed));
    CHECK(s.button_map[IN_GL] == OUT_NONE);
    // GL follows what the pressed button currently sends.
    s.button_map[IN_B] = OUT_X;
    CHECK(mapping_quick_remap(&s, chord, chord | S2_BTN_B, &changed));
    CHECK(s.button_map[IN_GL] == OUT_X);

    // GR works the same and leaves GL alone; both held is ambiguous; Home is skipped.
    uint32_t chord_r = S2_BTN_C | S2_BTN_GR;
    CHECK(mapping_quick_remap(&s, chord_r, chord_r | S2_BTN_UP, &changed));
    CHECK(changed == IN_GR && s.button_map[IN_GR] == OUT_UP && s.button_map[IN_GL] == OUT_X);
    CHECK(!mapping_quick_remap(&s, chord | S2_BTN_GR, chord | S2_BTN_GR | S2_BTN_Y, &changed));
    CHECK(!mapping_quick_remap(&s, chord, chord | S2_BTN_HOME, &changed));
}

static void test_macro(void) {
    settings_t s;
    test_defaults(&s);
    s.button_map[IN_C] = OUT_HOME_A;
    mapping_macro_t m;
    memset(&m, 0, sizeof m);
    uint32_t C = S2_BTN_C;

    // Tap C: nothing while held, then Home, Home + A, release.
    CHECK(mapping_macro_step(&m, &s, 0, C, 1000) == 0);
    CHECK(mapping_macro_busy(&m));
    CHECK(mapping_macro_step(&m, &s, C, C, 1100) == 0);
    CHECK(mapping_macro_step(&m, &s, C, 0, 1150) == S1_BTN_HOME);
    CHECK(mapping_macro_step(&m, &s, 0, 0, 1150 + MACRO_HOME_MS) == (S1_BTN_HOME | S1_BTN_A));
    CHECK(mapping_macro_step(&m, &s, 0, 0, 1150 + MACRO_HOME_MS + MACRO_HOME_A_MS) == 0);
    CHECK(!mapping_macro_busy(&m));

    // C used as a chord modifier (C + GL ...): no macro.
    CHECK(mapping_macro_step(&m, &s, 0, C, 2000) == 0);
    CHECK(mapping_macro_step(&m, &s, C, C | S2_BTN_GL, 2010) == 0);
    CHECK(mapping_macro_step(&m, &s, C | S2_BTN_GL, S2_BTN_GL, 2020) == 0);
    CHECK(!mapping_macro_busy(&m));
    // Pressed while another button is already held: no macro either.
    CHECK(mapping_macro_step(&m, &s, S2_BTN_A, S2_BTN_A | C, 3000) == 0);
    CHECK(mapping_macro_step(&m, &s, S2_BTN_A | C, S2_BTN_A, 3010) == 0);
    CHECK(!mapping_macro_busy(&m));
    // Not mapped: nothing.
    s.button_map[IN_C] = OUT_NONE;
    CHECK(mapping_macro_step(&m, &s, 0, C, 4000) == 0 && !mapping_macro_busy(&m));
    // The macro output itself sets no button through the normal mapping.
    CHECK(mapping_out_button_bit(OUT_HOME_A) == 0);
}

int main(void) {
    test_s1_rumble_classic();
    test_s1_rumble_packed();
    test_s2_rumble_encode();
    test_adv();
    test_commands();
    test_input_report();
    test_mapping();
    test_quick_remap();
    test_macro();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
