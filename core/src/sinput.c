#include "sinput.h"

#include <math.h>
#include <string.h>

#include "app.h"
#include "battery.h"
#include "hd_rumble.h"
#include "log.h"
#include "platform.h"
#include "s2_link.h"
#include "settings.h"
#include "usb_hid.h"

// Layouts follow SDL's SInput driver (SDL_hidapi_sinput.c) and GP2040-CE's
// SInput driver, which both follow Hand Held Legend's protocol documents.

// ---------------------------------------------------------------------------
// Report descriptor (hosts read the reports by ID and size)
// ---------------------------------------------------------------------------
static const uint8_t SINPUT_REPORT_DESC[] = {
    0x05, 0x01,             // Usage Page (Generic Desktop)
    0x09, 0x05,             // Usage (Game Pad)
    0xA1, 0x01,             // Collection (Application)
    0x85, 0x01,             //   Report ID (1): input, 63 bytes
    0x06, 0x00, 0xFF, 0x09, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,   // plug status, charge
    0x05, 0x09, 0x19, 0x01, 0x29, 0x20, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x20, 0x81, 0x02,   // 32 buttons
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x09, 0x33, 0x09, 0x34,               // sticks, triggers
    0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x06, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x09, 0x20, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x2C, 0x81, 0x02,   // IMU, touch, reserved
    0x85, 0x02, 0x09, 0x23, 0x95, 0x3F, 0x81, 0x02,   // input 0x02: command replies, 63 bytes
    0x85, 0x03, 0x09, 0x24, 0x95, 0x2F, 0x91, 0x02,   // output 0x03: commands, 47 bytes
    0xC0,
};

const uint8_t *sinput_report_descriptor(uint16_t *len) {
    *len = sizeof SINPUT_REPORT_DESC;
    return SINPUT_REPORT_DESC;
}

// ---------------------------------------------------------------------------
// Input report
// ---------------------------------------------------------------------------
// Button bits (bytes 3..6 as one little-endian word).
enum {
    SI_SOUTH = 0, SI_EAST, SI_WEST, SI_NORTH,
    SI_UP, SI_DOWN, SI_LEFT, SI_RIGHT,
    SI_L3, SI_R3, SI_L1, SI_R1, SI_L2, SI_R2,
    SI_L_PADDLE1, SI_R_PADDLE1,
    SI_START, SI_BACK, SI_GUIDE, SI_CAPTURE,
    SI_L_PADDLE2, SI_R_PADDLE2,
    SI_TOUCH1, SI_TOUCH2,
    SI_MISC,   // the first extra button (SDL: misc2, after Capture)
};

static const struct {
    uint8_t gp, bit;
} BUTTONS[] = {
    {GP_SOUTH, SI_SOUTH}, {GP_EAST, SI_EAST}, {GP_WEST, SI_WEST}, {GP_NORTH, SI_NORTH},
    {GP_UP, SI_UP}, {GP_DOWN, SI_DOWN}, {GP_LEFT, SI_LEFT}, {GP_RIGHT, SI_RIGHT},
    {GP_L3, SI_L3}, {GP_R3, SI_R3}, {GP_L1, SI_L1}, {GP_R1, SI_R1},
    {GP_PADDLE_L, SI_L_PADDLE1}, {GP_PADDLE_R, SI_R_PADDLE1},
    {GP_START, SI_START}, {GP_SELECT, SI_BACK}, {GP_GUIDE, SI_GUIDE}, {GP_MIC, SI_CAPTURE},
    {GP_FN_L, SI_L_PADDLE2}, {GP_FN_R, SI_R_PADDLE2},
    {GP_MISC, SI_MISC},
};

// The buttons the host is told about (usage masks). Always: face buttons,
// D-pad, stick clicks, bumpers, Start, Back, Guide. Triggers are analog only:
// with their digital bits too SDL would add two more buttons for them.
void sinput_usage_masks(const uint8_t map[IN_COUNT], uint32_t inputs, uint8_t m[4]) {
    uint32_t used = 0;
    for (int i = 0; i < IN_COUNT; i++) {
        if ((inputs & (1u << i)) && map[i] < GP_COUNT) used |= GP_BIT(map[i]);
    }
    bool pair1 = used & (GP_BIT(GP_PADDLE_L) | GP_BIT(GP_PADDLE_R));
    bool pair2 = used & (GP_BIT(GP_FN_L) | GP_BIT(GP_FN_R));
    m[0] = 0xFF;                                   // face, D-pad
    m[1] = 0x0F;                                   // L3 R3 L1 R1
    m[2] = 0x01 | 0x02 | 0x04;                     // Start Back Guide
    m[3] = 0;
    // SDL takes the second paddle pair (GL / GR) only together with the first.
    if (pair1 || pair2) m[1] |= 0xC0;
    if (pair2) m[2] |= 0x10 | 0x20;
    if (used & GP_BIT(GP_MIC)) m[2] |= 0x08;       // Capture
    if (used & GP_BIT(GP_MISC)) m[3] |= 0x01;      // C
}

static void put16(uint8_t *p, int v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (int)(v & 0xFFFF));
    put16(p + 2, (int)(v >> 16));
}

static int16_t sat16f(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)lrintf(v);
}

// 12-bit stick (y up) to SInput's signed 16 bits (y down, as SDL's axes).
static int16_t stick16(uint16_t v12, bool invert) {
    int v = ((int)v12 - S1_STICK_CENTER) * 32767 / S1_STICK_RANGE;
    if (invert) v = -v;
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

// 0..255 to -32768..32767 (released is the minimum).
static int16_t trigger16(uint8_t t) {
    return (int16_t)((int)t * 65535 / 255 - 32768);
}

void sinput_build_input(const sinput_state_t *st, uint32_t timestamp_us, uint8_t r[SINPUT_REPORT_LEN]) {
    memset(r, 0, SINPUT_REPORT_LEN);
    r[0] = 0x01;
    // Plug status: 2 charging, 4 on battery.
    r[1] = st->charging ? 2 : 4;
    r[2] = st->battery_pct > 100 ? 100 : st->battery_pct;
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof BUTTONS / sizeof BUTTONS[0]; i++) {
        if (st->gp & GP_BIT(BUTTONS[i].gp)) b |= 1u << BUTTONS[i].bit;
    }
    put32(r + 3, b);
    put16(r + 7, stick16(st->stick_l[0], false));
    put16(r + 9, stick16(st->stick_l[1], true));
    put16(r + 11, stick16(st->stick_r[0], false));
    put16(r + 13, stick16(st->stick_r[1], true));
    put16(r + 15, trigger16(st->trigger_l));
    put16(r + 17, trigger16(st->trigger_r));
    put32(r + 19, timestamp_us);
    // SDL reads SDL x = -X, SDL y = Z, SDL z = -Y (accelerometer and gyro).
    const float a = 32768.0f / SINPUT_ACCEL_RANGE_G, g = 32768.0f / SINPUT_GYRO_RANGE_DPS;
    put16(r + 23, sat16f(-st->accel_g[0] * a));
    put16(r + 25, sat16f(-st->accel_g[2] * a));
    put16(r + 27, sat16f(st->accel_g[1] * a));
    put16(r + 29, sat16f(-st->gyro_dps[0] * g));
    put16(r + 31, sat16f(-st->gyro_dps[2] * g));
    put16(r + 33, sat16f(st->gyro_dps[1] * g));
}

// ---------------------------------------------------------------------------
// Features reply
// ---------------------------------------------------------------------------
#define SINPUT_GAMEPAD_TYPE_SWITCH_PRO 7   // SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO
#define SINPUT_FACE_STYLE_BAYX         3   // Nintendo labels: B bottom, A right, Y left, X top
#define SINPUT_SUB_PRODUCT             0

void sinput_build_features(const uint8_t mac[6], const uint8_t masks[4], uint8_t r[SINPUT_REPORT_LEN]) {
    memset(r, 0, SINPUT_REPORT_LEN);
    r[0] = 0x02;
    r[1] = 0x02;                 // the features command
    uint8_t *d = r + 2;
    d[0] = 1;                    // protocol 1.0
    d[1] = 0;
    // Rumble, player LEDs, accelerometer, gyro, both sticks, both analog triggers.
    d[2] = 0x01 | 0x02 | 0x04 | 0x08 | 0x10 | 0x20 | 0x40 | 0x80;
    d[3] = 0;                    // no touchpad, no RGB, not a handheld
    d[4] = SINPUT_GAMEPAD_TYPE_SWITCH_PRO;
    d[5] = (uint8_t)((SINPUT_FACE_STYLE_BAYX << 5) | SINPUT_SUB_PRODUCT);
    uint8_t interval = g_settings.usb_report_interval_ms ? g_settings.usb_report_interval_ms : 4;
    put16(d + 6, interval * 1000);   // polling interval, µs
    put16(d + 8, SINPUT_ACCEL_RANGE_G);
    put16(d + 10, SINPUT_GYRO_RANGE_DPS);
    memcpy(d + 12, masks, 4);
    d[16] = 0;                   // touchpads
    d[17] = 0;
    memcpy(d + 18, mac, 6);
}

// ---------------------------------------------------------------------------
// Output report 0x03: [1] command, [2..] its data
// ---------------------------------------------------------------------------
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint8_t amp8(uint16_t a) {
    return (uint8_t)(a > 255 ? a >> 8 : a);
}

bool sinput_parse_output(const uint8_t *buf, uint16_t len, sinput_command_t *o) {
    memset(o, 0, sizeof *o);
    if (len < 2 || buf[0] != 0x03) return false;
    const uint8_t *d = buf + 2;
    uint16_t n = (uint16_t)(len - 2);
    switch (buf[1]) {
    case 0x01:   // haptics
        if (n >= 5 && d[0] == 2) {
            // ERM simulation: left (strong) and right (weak) amplitudes.
            o->rumble = true;
            o->motor_left = d[1];
            o->motor_right = d[3];
        } else if (n >= 17 && d[0] == 1) {
            // Frequency / amplitude pairs per side: the stronger of each.
            uint16_t l1 = le16(d + 3), l2 = le16(d + 7), r1 = le16(d + 11), r2 = le16(d + 15);
            o->rumble = true;
            o->motor_left = amp8(l1 > l2 ? l1 : l2);
            o->motor_right = amp8(r1 > r2 ? r1 : r2);
        }
        break;
    case 0x02: o->features = true; break;
    case 0x03:
        if (n >= 1) {
            o->player = true;
            o->player_num = d[0];
        }
        break;
    default: break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------
static sinput_state_t s_state;
static procon_status_t s_status;
static bool s_was_mounted;
static uint32_t s_next_report;
static uint8_t s_mac[6];
static bool s_features_due;
static bool s_features_sent;      // the host has the button list in s_sent_masks
static uint8_t s_sent_masks[4];
static uint8_t s_motor_l, s_motor_r;
static uint32_t s_rumble_refresh;

#define RUMBLE_REFRESH_MS 40

void sinput_init(void) {
    memset(&s_state, 0, sizeof s_state);
    memset(&s_status, 0, sizeof s_status);
    s_state.stick_l[0] = s_state.stick_l[1] = s_state.stick_r[0] = s_state.stick_r[1] = S1_STICK_CENTER;
    uint8_t id[PLATFORM_UNIQUE_ID_LEN];
    platform_unique_id(id);
    // Locally administered address derived from the chip ID.
    s_mac[0] = 0x02;
    for (int i = 1; i < 6; i++) s_mac[i] = id[i - 1] ^ id[i + 2];
}

void sinput_set_input(const s2_input_t *in, const mapping_ctx_t *ctx, uint32_t gp, bool connected) {
    sinput_state_t st;
    memset(&st, 0, sizeof st);
    st.stick_l[0] = st.stick_l[1] = st.stick_r[0] = st.stick_r[1] = S1_STICK_CENTER;
    st.battery_pct = s_state.battery_pct;
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
        st.battery_pct = battery_percent();
        st.charging = battery_charging();
    }
    s_state = st;
}

static void send_rumble(void) {
    rumble_sample_t l, r;
    rumble_from_motors(s_motor_l, s_motor_r, &l, &r);
    procon_hook_rumble(&l, 1, &r, 1);
    s_status.rumble_frames++;
}

void sinput_on_output(const uint8_t *buf, uint16_t len, bool via_control, uint8_t control_report_id) {
    uint8_t tmp[64];
    if (via_control && control_report_id && (len == 0 || buf[0] != control_report_id)) {
        // SET_REPORT data without the ID byte.
        if (len > sizeof tmp - 1) len = sizeof tmp - 1;
        tmp[0] = control_report_id;
        memcpy(tmp + 1, buf, len);
        buf = tmp;
        len = (uint16_t)(len + 1);
    }
    sinput_command_t c;
    if (!sinput_parse_output(buf, len, &c)) return;
    s_status.handshake_done = true;
    s_status.subcommands++;
    if (c.features) s_features_due = true;
    if (c.rumble && (c.motor_left != s_motor_l || c.motor_right != s_motor_r)) {
        s_motor_l = c.motor_left;
        s_motor_r = c.motor_right;
        send_rumble();
        s_rumble_refresh = platform_deadline_ms(RUMBLE_REFRESH_MS);
    }
    if (c.player && c.player_num != s_status.player_lights) {
        s_status.player_lights = c.player_num;
        int n = c.player_num > 4 ? 4 : c.player_num;
        if (n) procon_hook_player_lights((uint8_t)((1u << n) - 1));
    }
}

void sinput_task(void) {
    bool mounted = usb_hid_mounted();
    if (mounted != s_was_mounted) {
        s_was_mounted = mounted;
        s_motor_l = s_motor_r = 0;
        s_features_due = false;
        s_features_sent = false;
        s_status.handshake_done = false;
        LOG("sinput: USB %s", mounted ? "mounted" : "unmounted");
    }
    // The host sets rumble once and expects it to hold; keep it alive.
    if ((s_motor_l || s_motor_r) && platform_time_reached(s_rumble_refresh)) {
        send_rumble();
        s_rumble_refresh = platform_deadline_ms(RUMBLE_REFRESH_MS);
    }
    // The buttons the active profile uses (of the controller connected, or
    // the one the dongle starts with).
    ctrl_type_t t = s2_link_state() == S2_LINK_READY ? mapping_ctrl_type(s2_link_mapping_ctx())
                                                     : settings_boot_ctrl(&g_settings);
    uint8_t masks[4];
    sinput_usage_masks(settings_active_map(&g_settings, t), mapping_type_inputs(t), masks);
    // SDL reads the button list only when the device appears: when the list
    // itself changes (not just any mapping; see sinput_usage_masks()),
    // appear anew.
    // (After a moment: the page's or plugin's request that changed it gets
    // its answer first.)
    static bool changed;
    static uint32_t changed_at;
    if (mounted && s_features_sent && memcmp(masks, s_sent_masks, sizeof masks) != 0) {
        if (!changed) {
            changed = true;
            changed_at = platform_deadline_ms(500);
        } else if (platform_time_reached(changed_at)) {
            LOG("sinput: buttons changed, reconnecting USB");
            changed = false;
            s_features_sent = false;
            app_request_usb_reconnect();
            return;
        }
    } else {
        changed = false;
    }
    if (!mounted || !usb_hid_ready()) return;
    uint8_t r[SINPUT_REPORT_LEN];
    // The features reply goes first: SDL waits for it (100 ms) before it
    // reads any input.
    if (s_features_due) {
        sinput_build_features(s_mac, masks, r);
        if (usb_hid_send(r[0], r + 1, SINPUT_REPORT_LEN - 1)) {
            s_features_due = false;
            s_features_sent = true;
            memcpy(s_sent_masks, masks, sizeof masks);
        }
        return;
    }
    if (!platform_time_reached(s_next_report)) return;
    uint8_t interval = g_settings.usb_report_interval_ms ? g_settings.usb_report_interval_ms : 4;
    s_next_report = platform_deadline_ms(interval);
    sinput_build_input(&s_state, platform_millis() * 1000u, r);
    if (usb_hid_send(r[0], r + 1, SINPUT_REPORT_LEN - 1)) s_status.reports_sent++;
}

void sinput_get_status(procon_status_t *out) {
    *out = s_status;
    out->usb_mounted = usb_hid_mounted();
    out->report_mode = 0x01;
    out->vibration_enabled = true;
    out->imu_enabled = g_settings.gyro_enabled;
}
