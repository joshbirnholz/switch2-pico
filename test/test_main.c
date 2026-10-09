// Host-side unit tests for the hardware independent modules.
// Build & run: make -C test

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ds5.h"
#include "usb_mode.h"
#include "x360.h"
#include "mode_select.h"
#include "battery.h"
#include "gc_adapter.h"
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

// Stubs for ds5.c's runtime half (only its pure helpers are tested).
#include "platform.h"
#include "usb_hid.h"
uint32_t platform_millis(void) { return 0; }
void platform_unique_id(uint8_t out[PLATFORM_UNIQUE_ID_LEN]) { memset(out, 0x11, PLATFORM_UNIQUE_ID_LEN); }
bool usb_hid_mounted(void) { return false; }
bool usb_hid_ready(void) { return false; }
bool usb_hid_send(uint8_t id, const uint8_t *d, uint16_t n) { (void)id; (void)d; (void)n; return false; }
void procon_hook_rumble(const rumble_sample_t *l, int nl, const rumble_sample_t *r, int nr) { (void)l; (void)nl; (void)r; (void)nr; }
void procon_hook_player_lights(uint8_t lights) { (void)lights; }
void log_printf(const char *fmt, ...) { (void)fmt; }
usb_mode_t usb_mode_active(void) { return USB_MODE_DUALSENSE_EDGE; }

static void test_defaults(settings_t *s) {
    memset(s, 0, sizeof *s);
    for (int i = IN_A; i <= IN_RIGHT; i++) s->button_map[i] = (uint8_t)(OUT_A + i);
    memcpy(s->gc_profile.button_map, s->button_map, IN_COUNT);   // GameCube controller: same
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
    CHECK(!mapping_quick_remap(&s, CTRL_PRO, 0, S2_BTN_C, &changed));
    CHECK(!mapping_quick_remap(&s, CTRL_PRO, S2_BTN_C, chord, &changed));
    CHECK(mapping_quick_remap_held(chord) && !mapping_quick_remap_held(S2_BTN_GL));

    // C + GL + A: GL now sends A.
    CHECK(mapping_quick_remap(&s, CTRL_PRO, chord, chord | S2_BTN_A, &changed));
    CHECK(changed == IN_GL && s.button_map[IN_GL] == OUT_A);
    // Holding A doesn't repeat.
    CHECK(!mapping_quick_remap(&s, CTRL_PRO, chord | S2_BTN_A, chord | S2_BTN_A, &changed));
    // Same again clears it.
    CHECK(mapping_quick_remap(&s, CTRL_PRO, chord, chord | S2_BTN_A, &changed));
    CHECK(s.button_map[IN_GL] == OUT_NONE);
    // GL follows what the pressed button currently sends.
    s.button_map[IN_B] = OUT_X;
    CHECK(mapping_quick_remap(&s, CTRL_PRO, chord, chord | S2_BTN_B, &changed));
    CHECK(s.button_map[IN_GL] == OUT_X);

    // GR works the same and leaves GL alone; both held is ambiguous; Home is skipped.
    uint32_t chord_r = S2_BTN_C | S2_BTN_GR;
    CHECK(mapping_quick_remap(&s, CTRL_PRO, chord_r, chord_r | S2_BTN_UP, &changed));
    CHECK(changed == IN_GR && s.button_map[IN_GR] == OUT_UP && s.button_map[IN_GL] == OUT_X);
    CHECK(!mapping_quick_remap(&s, CTRL_PRO, chord | S2_BTN_GR, chord | S2_BTN_GR | S2_BTN_Y, &changed));
    CHECK(!mapping_quick_remap(&s, CTRL_PRO, chord, chord | S2_BTN_HOME, &changed));
}

static void test_macro(void) {
    settings_t s;
    test_defaults(&s);
    s.button_map[IN_C] = OUT_HOME_A;
    mapping_macro_t m;
    memset(&m, 0, sizeof m);
    uint32_t C = S2_BTN_C;

    // Tap C: nothing while held, then Home, Home + A, release.
    CHECK(mapping_macro_step(&m, &s, CTRL_PRO, 0, C, 1000) == 0);
    CHECK(mapping_macro_busy(&m));
    CHECK(mapping_macro_step(&m, &s, CTRL_PRO, C, C, 1100) == 0);
    CHECK(mapping_macro_step(&m, &s, CTRL_PRO, C, 0, 1150) == S1_BTN_HOME);
    CHECK(mapping_macro_step(&m, &s, CTRL_PRO, 0, 0, 1150 + MACRO_HOME_MS) == (S1_BTN_HOME | S1_BTN_A));
    CHECK(mapping_macro_step(&m, &s, CTRL_PRO, 0, 0, 1150 + MACRO_HOME_MS + MACRO_HOME_A_MS) == 0);
    CHECK(!mapping_macro_busy(&m));

    // C used as a chord modifier (C + GL ...): no macro.
    CHECK(mapping_macro_step(&m, &s, CTRL_PRO, 0, C, 2000) == 0);
    CHECK(mapping_macro_step(&m, &s, CTRL_PRO, C, C | S2_BTN_GL, 2010) == 0);
    CHECK(mapping_macro_step(&m, &s, CTRL_PRO, C | S2_BTN_GL, S2_BTN_GL, 2020) == 0);
    CHECK(!mapping_macro_busy(&m));
    // Pressed while another button is already held: no macro either.
    CHECK(mapping_macro_step(&m, &s, CTRL_PRO, S2_BTN_A, S2_BTN_A | C, 3000) == 0);
    CHECK(mapping_macro_step(&m, &s, CTRL_PRO, S2_BTN_A | C, S2_BTN_A, 3010) == 0);
    CHECK(!mapping_macro_busy(&m));
    // Not mapped: nothing.
    s.button_map[IN_C] = OUT_NONE;
    CHECK(mapping_macro_step(&m, &s, CTRL_PRO, 0, C, 4000) == 0 && !mapping_macro_busy(&m));
    // The macro output itself sets no button through the normal mapping.
    CHECK(mapping_out_button_bit(OUT_HOME_A) == 0);
}

static void test_ds5(void) {
    ds5_state_t st;
    memset(&st, 0, sizeof st);
    st.stick_l[0] = st.stick_l[1] = st.stick_r[0] = st.stick_r[1] = S1_STICK_CENTER;
    uint8_t r[DS5_INPUT_REPORT_LEN];

    // Neutral: centered sticks, hat released, no touches.
    ds5_build_input(&st, 7, 3000, r);
    CHECK(r[0] == 0x01 && r[1] == 128 && r[2] == 128 && r[3] == 128 && r[4] == 128);
    CHECK(r[7] == 7 && (r[8] & 0x0F) == 8 && r[9] == 0 && r[10] == 0);
    CHECK(r[33] == 0x80 && r[37] == 0x80);
    CHECK(r[28] == (3000 & 0xFF) && r[29] == (3000 >> 8));

    // Face buttons, d-pad, guide; full stick deflection (DualSense y up = 0).
    st.gp = GP_BIT(GP_EAST) | GP_BIT(GP_SOUTH) | GP_BIT(GP_UP) | GP_BIT(GP_RIGHT) | GP_BIT(GP_GUIDE) |
            GP_BIT(GP_L2) | GP_BIT(GP_PADDLE_L) | GP_BIT(GP_PADDLE_R) | GP_BIT(GP_FN_R) | GP_BIT(GP_MIC);
    st.stick_l[1] = S1_STICK_CENTER + S1_STICK_RANGE;
    st.stick_r[0] = S1_STICK_CENTER - S1_STICK_RANGE;
    st.trigger_l = 255;
    ds5_build_input(&st, 0, 0, r);
    CHECK((r[8] & 0x0F) == 1);              // up-right
    CHECK((r[8] & 0x40) && (r[8] & 0x20));  // circle, cross
    CHECK(!(r[8] & 0x10) && !(r[8] & 0x80));
    CHECK(r[9] & 0x04);                     // L2
    CHECK(r[10] == (0x01 | 0x04));          // PS, mic; no paddles / Fn on a plain DualSense
    st.edge = true;
    ds5_build_input(&st, 0, 0, r);
    CHECK(r[10] == (0x01 | 0x04 | 0x20 | 0x40 | 0x80));   // + right Fn, paddles
    CHECK(r[2] == 1 && r[3] == 1);
    CHECK(r[5] == 255);

    // Touchpad clicks: click bit plus a touch on the matching side.
    st.gp = GP_BIT(GP_TP_LEFT);
    st.touch_id = 5;
    ds5_build_input(&st, 0, 0, r);
    int x = r[34] | ((r[35] & 0x0F) << 8), y = (r[35] >> 4) | (r[36] << 4);
    CHECK((r[10] & 0x02) && r[33] == 5 && x < 960 && y == 540);
    st.gp = GP_BIT(GP_TP_RIGHT);
    ds5_build_input(&st, 0, 0, r);
    x = r[34] | ((r[35] & 0x0F) << 8);
    CHECK((r[10] & 0x02) && x > 960);
    st.gp = GP_BIT(GP_TOUCHPAD);
    ds5_build_input(&st, 0, 0, r);
    x = r[34] | ((r[35] & 0x0F) << 8);
    CHECK((r[10] & 0x02) && x == 960 && r[37] == 0x80);

    // Motion: 16 LSB per deg/s, 8192 LSB per g.
    st.gp = 0;
    st.gyro_dps[1] = 100.0f;
    st.accel_g[2] = -1.0f;
    ds5_build_input(&st, 0, 0, r);
    CHECK((int16_t)(r[18] | r[19] << 8) == 1600);
    CHECK((int16_t)(r[26] | r[27] << 8) == -8192);

    // Battery nibbles.
    st.battery_pct = 55;
    st.charging = true;
    ds5_build_input(&st, 0, 0, r);
    CHECK(r[53] == 0x15);

    // Output report 0x02: rumble (v1 and v2 flags) and player LEDs.
    uint8_t o[48];
    memset(o, 0, sizeof o);
    o[0] = 0x02;
    o[1] = 0x03;    // compatible vibration + haptics select
    o[3] = 40;      // right (weak)
    o[4] = 200;     // left (strong)
    o[2] = 0x10;    // player LED control
    o[44] = 0x0A;   // two LEDs
    ds5_output_t out;
    CHECK(ds5_parse_output(o, sizeof o, &out));
    CHECK(out.rumble && out.motor_left == 200 && out.motor_right == 40);
    CHECK(out.player_leds && out.player_pattern == 0x0A);
    o[1] = 0x02;    // haptics select only
    o[39] = 0x04;   // vibration v2 (SDL "enhanced rumble", Linux v2)
    CHECK(ds5_parse_output(o, sizeof o, &out) && out.rumble && out.motor_left == 200);
    // SDL's stop: the vibration bits off (values left as they were or zero).
    o[39] = 0;
    o[1] = 0;
    o[2] = 0;
    CHECK(ds5_parse_output(o, sizeof o, &out) && out.rumble && out.motor_left == 0 && out.motor_right == 0);
    CHECK(!out.player_leds);
    // SDL's "rumble start" (disable audio haptics only): still off.
    o[1] = 0x02;
    CHECK(ds5_parse_output(o, sizeof o, &out) && out.motor_left == 0 && out.motor_right == 0);
    o[0] = 0x31;
    CHECK(!ds5_parse_output(o, sizeof o, &out));

    // Feature reports: sizes the Linux driver insists on, calibration values.
    uint8_t f[64];
    CHECK(ds5_get_feature(0x05, f, 64) == 41 && f[0] == 0x05);
    CHECK((int16_t)(f[7] | f[8] << 8) == 1024 && (int16_t)(f[9] | f[10] << 8) == -1024);
    CHECK((f[19] | f[20] << 8) == 64 && (int16_t)(f[23] | f[24] << 8) == 8192);
    CHECK(ds5_get_feature(0x09, f, 64) == 20 && f[0] == 0x09);
    CHECK(ds5_get_feature(0x20, f, 64) == 64 && f[0] == 0x20 && (f[44] | f[45] << 8) == 0x0300);
    CHECK(ds5_get_feature(0x05, f, 10) == 10);   // never more than asked for
    uint16_t dl;
    const uint8_t *d = ds5_report_descriptor(&dl);
    CHECK(dl > 0 && d[0] == 0x05 && d[dl - 1] == 0xC0);
}

static void test_x360(void) {
    uint16_t c[2] = {S1_STICK_CENTER, S1_STICK_CENTER};
    uint16_t full[2] = {S1_STICK_CENTER + S1_STICK_RANGE, S1_STICK_CENTER - S1_STICK_RANGE};
    uint8_t r[X360_INPUT_LEN];
    x360_build_input(0, c, c, 0, 0, r);
    CHECK(r[0] == 0x00 && r[1] == 20 && r[2] == 0 && r[3] == 0 && r[6] == 0 && r[7] == 0);
    x360_build_input(GP_BIT(GP_SOUTH) | GP_BIT(GP_NORTH) | GP_BIT(GP_GUIDE) | GP_BIT(GP_L1) | GP_BIT(GP_UP) |
                         GP_BIT(GP_START) | GP_BIT(GP_R2),
                     full, c, 10, 0, r);
    CHECK(r[2] == (0x01 | 0x10));                    // up, start
    CHECK(r[3] == (0x01 | 0x04 | 0x10 | 0x80));      // LB, guide, A, Y
    CHECK(r[4] == 10 && r[5] == 255);
    CHECK((int16_t)(r[6] | r[7] << 8) == 32767);     // x right
    CHECK((int16_t)(r[8] | r[9] << 8) == -32767);    // y down (XInput y grows upwards)

    uint8_t rum[8] = {0x00, 0x08, 0x00, 200, 50, 0, 0, 0};
    x360_output_t o;
    CHECK(x360_parse_output(rum, sizeof rum, &o) && o.rumble && o.motor_left == 200 && o.motor_right == 50);
    uint8_t led[3] = {0x01, 0x03, 0x07};
    CHECK(x360_parse_output(led, sizeof led, &o) && o.led && o.player == 2);
    led[2] = 0x02;
    CHECK(x360_parse_output(led, sizeof led, &o) && o.player == 1);

    uint8_t desc[X360_ITF_DESC_LEN];
    x360_interface_desc(desc, 0, 0x81, 0x01);
    CHECK(desc[5] == 0xFF && desc[6] == 0x5D && desc[7] == 0x01);   // vendor, XInput
    CHECK(desc[9] == 17 && desc[10] == 0x21 && desc[15] == 0x81 && desc[22] == 0x01);
    CHECK(desc[26 + 2] == 0x81 && desc[33 + 2] == 0x01);
}

static void test_gp_map(void) {
    settings_t s;
    test_defaults(&s);
    uint8_t map[IN_COUNT];
    settings_default_mode_map(CTRL_PRO, USB_MODE_DUALSENSE_EDGE, map);
    CHECK(map[IN_A] == GP_EAST && map[IN_B] == GP_SOUTH && map[IN_GL] == GP_PADDLE_L && map[IN_C] == GP_FN_R);
    settings_default_mode_map(CTRL_PRO, USB_MODE_XBOX360, map);
    CHECK(map[IN_GL] == GP_NONE && map[IN_CAPTURE] == GP_NONE && map[IN_HOME] == GP_GUIDE);
    s2_input_t in;
    memset(&in, 0, sizeof in);
    in.buttons = S2_BTN_A | S2_BTN_ZL | S2_BTN_GL;
    map[IN_GL] = GP_MACRO_QAM;   // macros never set a button directly
    CHECK(mapping_gp_buttons(&s, map, NULL, &in) == (GP_BIT(GP_EAST) | GP_BIT(GP_L2)));
    // Quick remap works on generic maps: C + GR + Y -> GR = Y's output (West).
    in_button_t changed;
    uint32_t chord = S2_BTN_C | S2_BTN_GR;
    CHECK(mapping_quick_remap_map(map, chord, chord | S2_BTN_Y, &changed) && changed == IN_GR &&
          map[IN_GR] == GP_WEST);
    CHECK(mapping_quick_remap_map(map, chord, chord | S2_BTN_Y, &changed) && map[IN_GR] == GP_NONE);
    // Generic macro phases.
    mapping_macro_t m;
    memset(&m, 0, sizeof m);
    map[IN_C] = GP_MACRO_QAM;
    CHECK(mapping_macro_run(&m, map, GP_MACRO_QAM, 0, S2_BTN_C, 0) == MACRO_IDLE);
    CHECK(mapping_macro_run(&m, map, GP_MACRO_QAM, S2_BTN_C, 0, 10) == MACRO_GUIDE);
    CHECK(mapping_macro_run(&m, map, GP_MACRO_QAM, 0, 0, 10 + MACRO_HOME_MS) == MACRO_GUIDE_SOUTH);
}

static void test_imu_sdl(void) {
    settings_t s;
    test_defaults(&s);
    mapping_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gyro_lsb_per_dps = 16.0f;
    s2_input_t in;
    memset(&in, 0, sizeof in);
    in.accel[0] = 4096;    // +1 g on S2 x
    in.accel[1] = 2048;    // +0.5 g on S2 y
    in.accel[2] = -4096;
    in.gyro[0] = 160;      // 10 dps
    in.gyro[1] = 320;
    in.gyro[2] = -480;
    float a[3], g[3];
    mapping_imu_sdl(&s, &ctx, &in, a, g);
    // SDL frame = (S2 x, S2 z, -S2 y), as SDL's Switch 2 driver.
    CHECK_NEAR(a[0], 1.0, 1e-4);
    CHECK_NEAR(a[1], -1.0, 1e-4);
    CHECK_NEAR(a[2], -0.5, 1e-4);
    CHECK_NEAR(g[0], 10.0, 1e-3);
    CHECK_NEAR(g[1], -30.0, 1e-3);
    CHECK_NEAR(g[2], -20.0, 1e-3);
    s.gyro_enabled = 0;
    mapping_imu_sdl(&s, &ctx, &in, a, g);
    CHECK(a[0] == 0 && g[1] == 0);
}

static void test_mode_select(void) {
    const uint32_t CH = S2_BTN_C | S2_BTN_HOME;
    uint8_t slots[MODE_SLOT_COUNT];
    settings_default_mode_slots(slots);
    CHECK(slots[MODE_SLOT_A] == USB_MODE_DUALSENSE_EDGE + 1 && slots[MODE_SLOT_B] == USB_MODE_XBOX360 + 1);
    CHECK(slots[MODE_SLOT_X] == USB_MODE_DUALSENSE + 1 && slots[MODE_SLOT_Y] == USB_MODE_SWITCH_PRO + 1);
    CHECK(slots[MODE_SLOT_UP] == MODE_SLOT_EMPTY && slots[MODE_SLOT_RIGHT] == MODE_SLOT_EMPTY);
    mode_select_t m;
    usb_mode_t mode = USB_MODE_COUNT;
    mode_select_init(&m);
    // A short hold does nothing; the full hold enters once.
    CHECK(mode_select_update(&m, slots, CH, 1000, &mode) == MODE_SELECT_NONE);
    CHECK(mode_select_update(&m, slots, CH, 1000 + MODE_SELECT_HOLD_MS - 1, &mode) == MODE_SELECT_NONE);
    CHECK(mode_select_update(&m, slots, CH, 1000 + MODE_SELECT_HOLD_MS, &mode) == MODE_SELECT_ENTER);
    CHECK(m.active);
    CHECK(mode_select_update(&m, slots, CH, 4000, &mode) == MODE_SELECT_NONE);   // still holding: no cancel
    CHECK(mode_select_update(&m, slots, 0, 4100, &mode) == MODE_SELECT_NONE);
    // Empty D-pad slot: ignored, still active.
    CHECK(mode_select_update(&m, slots, S2_BTN_UP, 4200, &mode) == MODE_SELECT_NONE && m.active);
    CHECK(mode_select_update(&m, slots, 0, 4300, &mode) == MODE_SELECT_NONE);
    // B: Xbox 360.
    CHECK(mode_select_update(&m, slots, S2_BTN_B, 4400, &mode) == MODE_SELECT_CHOSEN);
    CHECK(mode == USB_MODE_XBOX360 && !m.active);

    // C + Home again leaves; so does the timeout.
    mode_select_init(&m);
    mode_select_update(&m, slots, CH, 0, &mode);
    CHECK(mode_select_update(&m, slots, CH, MODE_SELECT_HOLD_MS + 5, &mode) == MODE_SELECT_ENTER);
    CHECK(mode_select_update(&m, slots, 0, 2000, &mode) == MODE_SELECT_NONE);
    CHECK(mode_select_update(&m, slots, S2_BTN_C, 2100, &mode) == MODE_SELECT_NONE);
    CHECK(mode_select_update(&m, slots, CH, 2200, &mode) == MODE_SELECT_CANCEL);
    // Still held after leaving: no re-entry until released.
    CHECK(mode_select_update(&m, slots, CH, 9000, &mode) == MODE_SELECT_NONE && !m.active);
    mode_select_update(&m, slots, 0, 9100, &mode);
    mode_select_update(&m, slots, CH, 9200, &mode);
    CHECK(mode_select_update(&m, slots, CH, 9200 + MODE_SELECT_HOLD_MS, &mode) == MODE_SELECT_ENTER);
    CHECK(mode_select_update(&m, slots, 0, 11000, &mode) == MODE_SELECT_NONE);
    CHECK(mode_select_update(&m, slots, 0, 10700 + MODE_SELECT_TIMEOUT_MS - 1, &mode) == MODE_SELECT_NONE);
    CHECK(mode_select_update(&m, slots, 0, 10700 + MODE_SELECT_TIMEOUT_MS, &mode) == MODE_SELECT_CANCEL);

    // D-pad slot with a mode; no slots at all disables the shortcut.
    slots[MODE_SLOT_LEFT] = USB_MODE_DUALSENSE + 1;
    mode_select_init(&m);
    mode_select_update(&m, slots, CH, 0, &mode);
    mode_select_update(&m, slots, CH, MODE_SELECT_HOLD_MS, &mode);
    CHECK(mode_select_update(&m, slots, CH | S2_BTN_LEFT, MODE_SELECT_HOLD_MS + 10, &mode) == MODE_SELECT_CHOSEN);
    CHECK(mode == USB_MODE_DUALSENSE);
    memset(slots, MODE_SLOT_EMPTY, sizeof slots);
    CHECK(!mode_select_enabled(slots));
    mode_select_init(&m);
    mode_select_update(&m, slots, CH, 0, &mode);
    CHECK(mode_select_update(&m, slots, CH, 10000, &mode) == MODE_SELECT_NONE && !m.active);
}

static void test_classic_rumble(void) {
    rumble_sample_t l, r;
    s2_rumble_params_t p = {.translate_freq = true, .freq_slope = 117, .strength_pct = 100};
    uint8_t bl[S2_RUMBLE_BLOCK_LEN], br[S2_RUMBLE_BLOCK_LEN];
    // Left motor only: only the left actuator moves.
    rumble_from_motors(255, 0, &l, &r);
    CHECK(l.lo_amp > 0.9f && r.lo_amp == 0.0f && r.hi_amp == 0.0f);
    s2_rumble_encode_block(0, &l, 1, &p, bl);
    s2_rumble_encode_block(0, &r, 1, &p, br);
    CHECK(s2_rumble_block_active(bl) && !s2_rumble_block_active(br));
    // Right motor only: only the right actuator moves.
    rumble_from_motors(0, 255, &l, &r);
    CHECK(l.lo_amp == 0.0f && l.hi_amp == 0.0f && r.hi_amp > 0.5f);
    s2_rumble_encode_block(0, &l, 1, &p, bl);
    s2_rumble_encode_block(0, &r, 1, &p, br);
    CHECK(!s2_rumble_block_active(bl) && s2_rumble_block_active(br));
    // Strong vs weak: the left motor is the heavier, lower one.
    rumble_from_motors(255, 255, &l, &r);
    CHECK(l.lo_amp + l.hi_amp > r.lo_amp + r.hi_amp);
    CHECK(l.lo_freq_hz < r.lo_freq_hz && l.hi_freq_hz < r.hi_freq_hz);
    // Off is silent.
    rumble_from_motors(0, 0, &l, &r);
    s2_rumble_encode_block(0, &l, 1, &p, bl);
    CHECK(!s2_rumble_block_active(bl));
}

static void test_battery(void) {
    CHECK(battery_mv_to_percent(3300) == 0 && battery_mv_to_percent(4200) == 100);
    CHECK(battery_mv_to_percent(3700) == 30 && battery_mv_to_percent(3725) == 36);
    CHECK(battery_mv_to_percent(3900) > battery_mv_to_percent(3800));
    battery_t b;
    battery_reset(&b);
    battery_update(&b, 3900, false, 0);
    CHECK(b.pct == 72);
    // A rumble dip of 150 mV for 200 ms barely moves it.
    for (uint32_t t = 8; t <= 200; t += 8) battery_update(&b, 3750, false, t);
    CHECK(b.pct >= 70);
    // Back up: discharging never rises by a small amount.
    for (uint32_t t = 208; t <= 20000; t += 8) battery_update(&b, 3900, false, t);
    CHECK(b.pct <= 72 && b.pct >= 70);
    // A sustained drop goes through.
    for (uint32_t t = 20008; t <= 80000; t += 8) battery_update(&b, 3700, false, t);
    CHECK(b.pct >= 29 && b.pct <= 31);
    // Charger connected: the reading is corrected for the charging voltage.
    battery_update(&b, 3900, true, 80008);
    CHECK(b.charging && b.pct == battery_mv_to_percent(3780));
    // No reading (0 mV) is ignored.
    uint8_t p = b.pct;
    battery_update(&b, 0, true, 80016);
    CHECK(b.pct == p);
}

static void test_gc_adapter(void) {
    uint8_t r[GC_REPORT_LEN];
    // No controller: four empty ports with rumble power.
    gc_build_report(NULL, r);
    CHECK(r[0] == 0x21 && r[1] == 0x04 && r[10] == 0x04 && r[19] == 0x04 && r[28] == 0x04 && r[2] == 0);
    // Controller in port 1.
    gc_port_t p = {.connected = true, .stick = {128, 228}, .cstick = {28, 128}, .l = 200, .r = 0};
    p.gp = GP_BIT(GP_SOUTH) | GP_BIT(GP_NORTH) | GP_BIT(GP_UP) | GP_BIT(GP_START) | GP_BIT(GP_R1) | GP_BIT(GP_L2);
    gc_build_report(&p, r);
    CHECK(r[1] == 0x14);
    CHECK(r[2] == (0x01 | 0x08 | 0x80));          // A, Y, D-up
    CHECK(r[3] == (0x01 | 0x02 | 0x08));          // Start, Z, L
    CHECK(r[4] == 128 && r[5] == 228 && r[6] == 28 && r[7] == 128 && r[8] == 200 && r[9] == 0);
    CHECK(r[10] == 0x04);                          // port 2 empty
    // Stick scaling: center, full deflection = +-100, clamped.
    CHECK(gc_axis(S1_STICK_CENTER) == 128);
    CHECK(gc_axis(S1_STICK_CENTER + S1_STICK_RANGE) == 228 && gc_axis(S1_STICK_CENTER - S1_STICK_RANGE) == 28);
    CHECK(gc_axis(4095) == 241 && gc_axis(0) == 15);   // beyond full deflection: no wrap
    // Host commands.
    gc_output_t o;
    const uint8_t start[] = {0x13};
    CHECK(gc_parse_output(start, 1, &o) && o.start && !o.rumble);
    const uint8_t on[] = {0x11, 0x01, 0x00, 0x00, 0x00}, brake[] = {0x11, 0x02, 0, 0, 0};
    CHECK(gc_parse_output(on, 5, &o) && o.rumble && o.rumble_on);
    CHECK(gc_parse_output(brake, 5, &o) && o.rumble && !o.rumble_on);
    const uint8_t bad[] = {0x55};
    CHECK(!gc_parse_output(bad, 1, &o));
    // Report descriptor: same length as the real adapter's.
    uint16_t len;
    gc_adapter_report_descriptor(&len);
    CHECK(len == 214);
    // Default map: by label.
    uint8_t map[IN_COUNT];
    settings_default_mode_map(CTRL_GAMECUBE, USB_MODE_GC_ADAPTER, map);
    CHECK(map[IN_A] == GP_SOUTH && map[IN_B] == GP_WEST && map[IN_ZR] == GP_R1 && map[IN_PLUS] == GP_START);
    CHECK(map[IN_L] == GP_L2 && map[IN_R] == GP_R2 && map[IN_HOME] == GP_NONE);
    // Pro Controller in GameCube adapter mode: triggers are L / R, R is Z.
    settings_default_mode_map(CTRL_PRO, USB_MODE_GC_ADAPTER, map);
    CHECK(map[IN_ZL] == GP_L2 && map[IN_ZR] == GP_R2 && map[IN_R] == GP_R1 && map[IN_L] == GP_NONE);
    // GameCube controller in a gamepad mode: analog L / R are the triggers, Z / ZL the bumpers.
    settings_default_mode_map(CTRL_GAMECUBE, USB_MODE_XBOX360, map);
    CHECK(map[IN_L] == GP_L2 && map[IN_R] == GP_R2 && map[IN_ZR] == GP_R1 && map[IN_ZL] == GP_L1);
    settings_default_mode_map(CTRL_PRO, USB_MODE_XBOX360, map);
    CHECK(map[IN_L] == GP_L1 && map[IN_ZR] == GP_R2);
    // Profiles are separate; the map lookup follows the controller type.
    static settings_t st;
    memset(&st, 0, sizeof st);
    settings_default_profile(CTRL_GAMECUBE, &st.gc_profile);
    settings_default_button_map(CTRL_PRO, st.button_map);
    st.button_map[IN_A] = OUT_B;
    CHECK(settings_button_map(&st, CTRL_PRO)[IN_A] == OUT_B && settings_button_map(&st, CTRL_GAMECUBE)[IN_A] == OUT_A);
    CHECK(settings_map_for(&st, CTRL_GAMECUBE, USB_MODE_XBOX360) == st.gc_profile.mode_map[USB_MODE_XBOX360]);
    mapping_ctx_t gcx;
    memset(&gcx, 0, sizeof gcx);
    gcx.is_gamecube = true;
    CHECK(mapping_ctrl_type(&gcx) == CTRL_GAMECUBE && mapping_ctrl_type(NULL) == CTRL_PRO);
    // Stick and rumble settings are per controller too.
    st.stick_deadzone_pct = 6;
    st.rumble_strength_pct = 100;
    st.gc_profile.stick_deadzone_pct = 12;
    st.gc_profile.rumble_strength_pct = 40;
    CHECK(settings_tuning(&st, CTRL_PRO).deadzone == 6 && settings_tuning(&st, CTRL_PRO).rumble_strength == 100);
    CHECK(settings_tuning(&st, CTRL_GAMECUBE).deadzone == 12 && settings_tuning(&st, CTRL_GAMECUBE).rumble_strength == 40);
    // USB mode per controller type; the dongle starts in the last-connected type's.
    st.usb_mode = USB_MODE_XBOX360;
    st.gc_usb_mode = USB_MODE_GC_ADAPTER;
    st.last_ctrl = CTRL_PRO;
    CHECK(settings_boot_usb_mode(&st) == USB_MODE_XBOX360);
    st.last_ctrl = CTRL_GAMECUBE;
    CHECK(settings_boot_usb_mode(&st) == USB_MODE_GC_ADAPTER);
    *settings_usb_mode(&st, CTRL_GAMECUBE) = USB_MODE_DUALSENSE;
    CHECK(st.gc_usb_mode == USB_MODE_DUALSENSE && st.usb_mode == USB_MODE_XBOX360);
}

static void test_bonds(void) {
    static settings_t st;
    memset(&st, 0, sizeof st);
    uint8_t a[BOND_MAX + 1][6];
    for (int i = 0; i <= BOND_MAX; i++) {
        memset(a[i], 0, 6);
        a[i][5] = (uint8_t)(i + 1);
    }
    bond_t old;
    for (int i = 0; i < BOND_MAX; i++) CHECK(!settings_bond_add(&st, a[i], 0, 0x2069, &old));
    CHECK(settings_bond_count(&st) == BOND_MAX);
    // All remembered, newest first; the single-controller fields follow the newest.
    for (int i = 0; i < BOND_MAX; i++) CHECK(settings_bond_find(&st, a[i]) == BOND_MAX - 1 - i);
    CHECK(st.bonded && memcmp(st.ctrl_addr, a[BOND_MAX - 1], 6) == 0);
    // Re-pairing a known one moves it to the front without dropping anything.
    CHECK(!settings_bond_add(&st, a[0], 0, 0x2073, &old));
    CHECK(settings_bond_find(&st, a[0]) == 0 && st.ctrl_pid == 0x2073 && settings_bond_count(&st) == BOND_MAX);
    // One more when full forgets the oldest pairing (a[1] now).
    CHECK(settings_bond_add(&st, a[BOND_MAX], 0, 0x2069, &old));
    CHECK(memcmp(old.addr, a[1], 6) == 0 && settings_bond_find(&st, a[1]) < 0);
    CHECK(settings_bond_find(&st, a[0]) == 1 && settings_bond_count(&st) == BOND_MAX);
    // Forget one, then all.
    CHECK(settings_bond_remove(&st, a[BOND_MAX]) && !settings_bond_remove(&st, a[BOND_MAX]));
    CHECK(settings_bond_find(&st, a[0]) == 0 && memcmp(st.ctrl_addr, a[0], 6) == 0);
    CHECK(settings_bond_count(&st) == BOND_MAX - 1 && !st.bonds[BOND_MAX - 1].used);
    settings_bond_clear(&st);
    CHECK(!st.bonded && settings_bond_count(&st) == 0);
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
    test_ds5();
    test_x360();
    test_gp_map();
    test_imu_sdl();
    test_mode_select();
    test_classic_rumble();
    test_battery();
    test_gc_adapter();
    test_bonds();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
