#include "ds5.h"

#include "battery.h"

#include <math.h>
#include <string.h>

#include "log.h"
#include "platform.h"
#include "settings.h"
#include "usb_hid.h"
#include "usb_mode.h"

// Layouts follow the DualSense USB reports as documented by SDL
// (SDL_hidapi_ps5.c) and the Linux hid-playstation driver.

// ---------------------------------------------------------------------------
// Report descriptor (our own; only the report IDs and sizes matter to hosts)
// ---------------------------------------------------------------------------
static const uint8_t DS5_REPORT_DESC[] = {
    0x05, 0x01,             // Usage Page (Generic Desktop)
    0x09, 0x05,             // Usage (Game Pad)
    0xA1, 0x01,             // Collection (Application)
    0x85, 0x01,             //   Report ID (1): input, 63 bytes
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x09, 0x33, 0x09, 0x34,   // X Y Z Rz Rx Ry
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x09, 0x20, 0x95, 0x01, 0x81, 0x02,                     // sequence number
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01,
    0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,                           // hat
    0x65, 0x00, 0x45, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x14, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x14, 0x81, 0x02,   // 20 buttons
    0x06, 0x00, 0xFF, 0x09, 0x21, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x35, 0x81, 0x02,   // 53 bytes: motion, touch, status
    0x85, 0x02, 0x09, 0x22, 0x95, 0x3E, 0x91, 0x02,   // output 0x02, 62 bytes (hosts send 48 or 63 with the ID)
    0x85, 0x05, 0x09, 0x23, 0x95, 0x28, 0xB1, 0x02,   // feature 0x05 calibration, 40 bytes
    0x85, 0x09, 0x09, 0x24, 0x95, 0x13, 0xB1, 0x02,   // feature 0x09 pairing info, 19 bytes
    0x85, 0x20, 0x09, 0x25, 0x95, 0x3F, 0xB1, 0x02,   // feature 0x20 firmware info, 63 bytes
    0xC0,
};

const uint8_t *ds5_report_descriptor(uint16_t *len) {
    *len = sizeof DS5_REPORT_DESC;
    return DS5_REPORT_DESC;
}

// ---------------------------------------------------------------------------
// Feature reports
// ---------------------------------------------------------------------------
static uint8_t s_mac[6];   // little-endian, as the pairing report carries it

static void put16(uint8_t *p, int v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (int)(v & 0xFFFF));
    put16(p + 2, (int)(v >> 16));
}

// Calibration chosen so the raw values we send are 16 LSB per deg/s and
// 8192 LSB per g with no bias (both drivers scale by speed and range).
#define DS5_GYRO_LSB_PER_DPS 16.0f
#define DS5_ACCEL_LSB_PER_G  8192.0f

uint16_t ds5_get_feature(uint8_t report_id, uint8_t *buf, uint16_t len) {
    uint8_t r[64];
    uint16_t n;
    memset(r, 0, sizeof r);
    r[0] = report_id;
    switch (report_id) {
    case 0x05:   // calibration
        // [1..6] gyro bias = 0; [7..18] gyro pitch/yaw/roll plus/minus;
        // [19..22] gyro speed plus/minus; [23..34] accel x/y/z plus/minus.
        for (int i = 0; i < 3; i++) {
            put16(r + 7 + i * 4, 1024);
            put16(r + 9 + i * 4, -1024);
        }
        put16(r + 19, 64);
        put16(r + 21, 64);
        for (int i = 0; i < 3; i++) {
            put16(r + 23 + i * 4, 8192);
            put16(r + 25 + i * 4, -8192);
        }
        n = 41;
        break;
    case 0x09:   // pairing info: the controller's MAC
        memcpy(r + 1, s_mac, 6);
        n = 20;
        break;
    case 0x20:   // firmware info
        memcpy(r + 1, "Jan  1 2026", 11);
        memcpy(r + 12, "00:00:00", 8);
        put16(r + 20, 0x0002);          // firmware type
        put16(r + 22, usb_mode_active() == USB_MODE_DUALSENSE_EDGE ? 0x0044 : 0x0004);   // software series
        put32(r + 24, 0x01000216);      // hardware info
        put32(r + 28, 0x01000100);      // firmware version
        put16(r + 44, 0x0300);          // update version
        n = 64;
        break;
    default:
        n = len < sizeof r ? len : (uint16_t)sizeof r;
        break;
    }
    if (n > len) n = len;
    memcpy(buf, r, n);
    return n;
}

// ---------------------------------------------------------------------------
// Input report
// ---------------------------------------------------------------------------
static uint8_t stick8(uint16_t v12, bool invert) {
    int v = ((int)v12 - S1_STICK_CENTER) * 127 / S1_STICK_RANGE;
    if (invert) v = -v;
    v += 128;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static int16_t sat16f(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)lrintf(v);
}

static uint8_t hat(uint32_t b) {
    bool up = b & GP_BIT(GP_UP), down = b & GP_BIT(GP_DOWN), left = b & GP_BIT(GP_LEFT), right = b & GP_BIT(GP_RIGHT);
    if (up && right) return 1;
    if (down && right) return 3;
    if (down && left) return 5;
    if (up && left) return 7;
    if (up) return 0;
    if (right) return 2;
    if (down) return 4;
    if (left) return 6;
    return 8;
}

#define TP_W 1920
#define TP_H 1080

static void touch_point(uint8_t *p, bool down, uint8_t id, int x, int y) {
    p[0] = (uint8_t)(down ? (id & 0x7F) : 0x80);   // bit 7 set: not touching
    p[1] = (uint8_t)x;
    p[2] = (uint8_t)(((x >> 8) & 0x0F) | ((y & 0x0F) << 4));
    p[3] = (uint8_t)(y >> 4);
}

void ds5_build_input(const ds5_state_t *st, uint8_t seq, uint32_t timestamp, uint8_t r[DS5_INPUT_REPORT_LEN]) {
    memset(r, 0, DS5_INPUT_REPORT_LEN);
    uint32_t b = st->gp;
#define ON(g, bit) ((b & GP_BIT(g)) ? (bit) : 0)
    r[0] = 0x01;
    // DualSense Y axes grow downwards.
    r[1] = stick8(st->stick_l[0], false);
    r[2] = stick8(st->stick_l[1], true);
    r[3] = stick8(st->stick_r[0], false);
    r[4] = stick8(st->stick_r[1], true);
    r[5] = st->trigger_l;
    r[6] = st->trigger_r;
    r[7] = seq;
    r[8] = (uint8_t)(hat(b) | ON(GP_WEST, 0x10) | ON(GP_SOUTH, 0x20) | ON(GP_EAST, 0x40) | ON(GP_NORTH, 0x80));
    r[9] = (uint8_t)(ON(GP_L1, 0x01) | ON(GP_R1, 0x02) | (b & GP_BIT(GP_L2) || st->trigger_l > 30 ? 0x04 : 0) |
                     (b & GP_BIT(GP_R2) || st->trigger_r > 30 ? 0x08 : 0) | ON(GP_SELECT, 0x10) | ON(GP_START, 0x20) |
                     ON(GP_L3, 0x40) | ON(GP_R3, 0x80));
    bool tp_l = b & GP_BIT(GP_TP_LEFT), tp_c = b & GP_BIT(GP_TOUCHPAD), tp_r = b & GP_BIT(GP_TP_RIGHT);
    r[10] = (uint8_t)(ON(GP_GUIDE, 0x01) | (tp_l || tp_c || tp_r ? 0x02 : 0) | ON(GP_MIC, 0x04));
    if (st->edge) {
        r[10] |= (uint8_t)(ON(GP_FN_L, 0x10) | ON(GP_FN_R, 0x20) | ON(GP_PADDLE_L, 0x40) | ON(GP_PADDLE_R, 0x80));
    }
#undef ON
    for (int i = 0; i < 3; i++) {
        put16(r + 16 + i * 2, sat16f(st->gyro_dps[i] * DS5_GYRO_LSB_PER_DPS));
        put16(r + 22 + i * 2, sat16f(st->accel_g[i] * DS5_ACCEL_LSB_PER_G));
    }
    put32(r + 28, timestamp);
    // A touchpad click also needs a finger on the pad where it clicks: hosts
    // (Steam) tell left / right clicks apart by the touch position.
    int x = tp_l ? TP_W / 4 : tp_r ? TP_W * 3 / 4 : TP_W / 2;
    touch_point(r + 33, tp_l || tp_c || tp_r, st->touch_id, x, TP_H / 2);
    touch_point(r + 37, false, 0, 0, 0);
    // Battery: low nibble in tens of percent, high nibble 0 discharging / 1 charging / 2 full.
    uint8_t level = (uint8_t)(st->battery_pct >= 100 ? 10 : st->battery_pct / 10);
    uint8_t status = st->charging ? (st->battery_pct >= 100 ? 2 : 1) : 0;
    r[53] = (uint8_t)(level | (status << 4));
}

// ---------------------------------------------------------------------------
// Output report 0x02 (layout after the ID: valid_flag0, valid_flag1,
// motor_right, motor_left, ..., valid_flag2 @38, player_leds @43)
// ---------------------------------------------------------------------------
bool ds5_parse_output(const uint8_t *buf, uint16_t len, ds5_output_t *o) {
    memset(o, 0, sizeof *o);
    if (len < 5 || buf[0] != 0x02) return false;
    const uint8_t *c = buf + 1;
    uint8_t flag2 = len > 39 ? c[38] : 0;
    // Every report sets the rumble: the motor values count only while
    // compatible vibration is enabled (v1: flag0 bit 0; v2: flag2 bit 2).
    // Hosts stop rumble by sending a report with those bits off (SDL:
    // "leaving emulated rumble bits off"), as a real DualSense expects.
    o->rumble = true;
    if ((c[0] & 0x01) || (flag2 & 0x04)) {
        o->motor_right = c[2];
        o->motor_left = c[3];
    }
    if ((c[1] & 0x10) && len > 44) {
        o->player_leds = true;
        o->player_pattern = c[43] & 0x1F;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------
static ds5_state_t s_state;
static procon_status_t s_status;
static bool s_was_mounted;
static uint32_t s_next_report;
static uint8_t s_seq;
static uint8_t s_motor_l, s_motor_r;
static uint32_t s_rumble_refresh;

#define RUMBLE_REFRESH_MS 40

void ds5_init(void) {
    memset(&s_state, 0, sizeof s_state);
    memset(&s_status, 0, sizeof s_status);
    s_state.stick_l[0] = s_state.stick_l[1] = s_state.stick_r[0] = s_state.stick_r[1] = S1_STICK_CENTER;
    uint8_t id[PLATFORM_UNIQUE_ID_LEN];
    platform_unique_id(id);
    // Locally administered address derived from the chip ID.
    s_mac[5] = 0x02;
    for (int i = 0; i < 5; i++) s_mac[i] = id[i] ^ id[i + 3];
}


void ds5_set_input(const s2_input_t *in, const mapping_ctx_t *ctx, uint32_t gp, bool connected) {
    ds5_state_t st;
    memset(&st, 0, sizeof st);
    st.edge = usb_mode_active() == USB_MODE_DUALSENSE_EDGE;
    st.stick_l[0] = st.stick_l[1] = st.stick_r[0] = st.stick_r[1] = S1_STICK_CENTER;
    st.battery_pct = s_state.battery_pct;
    st.touch_id = s_state.touch_id;
    if (in && connected) {
        st.gp = gp;
        procon_input_t tmp;
        mapping_apply(&g_settings, ctx, in, &tmp);
        memcpy(st.stick_l, tmp.stick_l, sizeof st.stick_l);
        memcpy(st.stick_r, tmp.stick_r, sizeof st.stick_r);
        if (ctx && ctx->is_gamecube) {
            int l = (int)in->trigger_l - ctx->gc_trigger_neutral[0];
            int r = (int)in->trigger_r - ctx->gc_trigger_neutral[1];
            int span_l = 255 - ctx->gc_trigger_neutral[0], span_r = 255 - ctx->gc_trigger_neutral[1];
            st.trigger_l = (uint8_t)(l <= 0 ? 0 : l * 255 / (span_l > 0 ? span_l : 255));
            st.trigger_r = (uint8_t)(r <= 0 ? 0 : r * 255 / (span_r > 0 ? span_r : 255));
        }
        if (gp & GP_BIT(GP_L2)) st.trigger_l = 255;
        if (gp & GP_BIT(GP_R2)) st.trigger_r = 255;
        mapping_imu_sdl(&g_settings, ctx, in, st.accel_g, st.gyro_dps);
        st.battery_pct = battery_percent();   // smoothed, see battery.c
        st.charging = battery_charging();
    }
    // A new touch gets a new tracking id.
    const uint32_t touch = GP_BIT(GP_TOUCHPAD) | GP_BIT(GP_TP_LEFT) | GP_BIT(GP_TP_RIGHT);
    if ((st.gp & touch) && !(s_state.gp & touch)) st.touch_id = (uint8_t)((s_state.touch_id + 1) & 0x7F);
    s_state = st;
}

static void send_rumble(void) {
    rumble_sample_t l, r;
    rumble_from_motors(s_motor_l, s_motor_r, &l, &r);
    procon_hook_rumble(&l, 1, &r, 1);
    s_status.rumble_frames++;
}

void ds5_on_output(const uint8_t *buf, uint16_t len, bool via_control, uint8_t control_report_id) {
    uint8_t tmp[64];
    if (via_control && control_report_id && (len == 0 || buf[0] != control_report_id)) {
        // SET_REPORT data without the ID byte.
        if (len > sizeof tmp - 1) len = sizeof tmp - 1;
        tmp[0] = control_report_id;
        memcpy(tmp + 1, buf, len);
        buf = tmp;
        len = (uint16_t)(len + 1);
    }
    ds5_output_t o;
    if (!ds5_parse_output(buf, len, &o)) return;
    s_status.handshake_done = true;
    if (o.rumble && (o.motor_left != s_motor_l || o.motor_right != s_motor_r)) {
        s_motor_l = o.motor_left;
        s_motor_r = o.motor_right;
        send_rumble();
        s_rumble_refresh = platform_deadline_ms(RUMBLE_REFRESH_MS);
    }
    if (o.player_leds && o.player_pattern != s_status.player_lights) {
        s_status.player_lights = o.player_pattern;
        int n = __builtin_popcount(o.player_pattern);
        if (n > 4) n = 4;
        if (n) procon_hook_player_lights((uint8_t)((1u << n) - 1));
    }
}

void ds5_task(void) {
    bool mounted = usb_hid_mounted();
    if (mounted != s_was_mounted) {
        s_was_mounted = mounted;
        s_motor_l = s_motor_r = 0;
        s_status.handshake_done = false;
        LOG("ds5: USB %s", mounted ? "mounted" : "unmounted");
    }
    // The host sets rumble once and expects it to hold; keep it alive.
    if ((s_motor_l || s_motor_r) && platform_time_reached(s_rumble_refresh)) {
        send_rumble();
        s_rumble_refresh = platform_deadline_ms(RUMBLE_REFRESH_MS);
    }
    if (!mounted || !usb_hid_ready() || !platform_time_reached(s_next_report)) return;
    uint8_t interval = g_settings.usb_report_interval_ms ? g_settings.usb_report_interval_ms : 4;
    s_next_report = platform_deadline_ms(interval);
    uint8_t r[DS5_INPUT_REPORT_LEN];
    ds5_build_input(&s_state, s_seq, platform_millis() * 3000u, r);
    if (usb_hid_send(r[0], r + 1, DS5_INPUT_REPORT_LEN - 1)) {
        s_seq++;
        s_status.reports_sent++;
    }
}

void ds5_get_status(procon_status_t *out) {
    *out = s_status;
    out->usb_mounted = usb_hid_mounted();
    out->report_mode = 0x01;
    out->vibration_enabled = true;
    out->imu_enabled = g_settings.gyro_enabled;
}
