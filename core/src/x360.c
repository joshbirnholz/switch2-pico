#include "x360.h"

#include <string.h>

#include "log.h"
#include "platform.h"
#include "settings.h"
#include "usb_hid.h"

// Report layouts as handled by Linux xpad and SDL's XInput / xbox360 drivers.

void x360_interface_desc(uint8_t d[X360_ITF_DESC_LEN], uint8_t itf, uint8_t ep_in, uint8_t ep_out) {
    const uint8_t desc[X360_ITF_DESC_LEN] = {
        // Interface: vendor class, XInput subclass / protocol, 2 endpoints
        9, 0x04, itf, 0, 2, 0xFF, 0x5D, 0x01, 0,
        // XInput "unknown" class descriptor (type 0x21) naming the endpoints
        17, 0x21, 0x00, 0x01, 0x01, 0x25, ep_in, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, ep_out, 0x08, 0x00, 0x00,
        // Interrupt IN, 32 bytes, 4 ms; interrupt OUT, 32 bytes, 8 ms
        7, 0x05, ep_in, 0x03, X360_EP_SIZE, 0, 4,
        7, 0x05, ep_out, 0x03, X360_EP_SIZE, 0, 8,
    };
    memcpy(d, desc, sizeof desc);
}

static int16_t axis16(uint16_t v12) {
    long v = ((long)v12 - S1_STICK_CENTER) * 32767 / S1_STICK_RANGE;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static void put16(uint8_t *p, int16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)((uint16_t)v >> 8);
}

void x360_build_input(uint32_t gp, const uint16_t stick_l[2], const uint16_t stick_r[2], uint8_t lt, uint8_t rt,
                      uint8_t r[X360_INPUT_LEN]) {
#define ON(g, bit) ((gp & GP_BIT(g)) ? (bit) : 0)
    memset(r, 0, X360_INPUT_LEN);
    r[0] = 0x00;    // message type: input
    r[1] = X360_INPUT_LEN;
    r[2] = (uint8_t)(ON(GP_UP, 0x01) | ON(GP_DOWN, 0x02) | ON(GP_LEFT, 0x04) | ON(GP_RIGHT, 0x08) |
                     ON(GP_START, 0x10) | ON(GP_SELECT, 0x20) | ON(GP_L3, 0x40) | ON(GP_R3, 0x80));
    r[3] = (uint8_t)(ON(GP_L1, 0x01) | ON(GP_R1, 0x02) | ON(GP_GUIDE, 0x04) | ON(GP_SOUTH, 0x10) |
                     ON(GP_EAST, 0x20) | ON(GP_WEST, 0x40) | ON(GP_NORTH, 0x80));
#undef ON
    r[4] = (gp & GP_BIT(GP_L2)) ? 255 : lt;
    r[5] = (gp & GP_BIT(GP_R2)) ? 255 : rt;
    // XInput Y axes grow upwards, like ours.
    put16(r + 6, axis16(stick_l[0]));
    put16(r + 8, axis16(stick_l[1]));
    put16(r + 10, axis16(stick_r[0]));
    put16(r + 12, axis16(stick_r[1]));
}

bool x360_parse_output(const uint8_t *buf, uint16_t len, x360_output_t *o) {
    memset(o, 0, sizeof *o);
    if (len < 3) return false;
    if (buf[0] == 0x00 && len >= 5) {   // rumble: 00 08 00 <left> <right> 00 00 00
        o->rumble = true;
        o->motor_left = buf[3];
        o->motor_right = buf[4];
        return true;
    }
    if (buf[0] == 0x01) {               // LED ring: 01 03 <pattern>
        uint8_t p = buf[2];
        o->led = true;
        if (p >= 0x02 && p <= 0x05) o->player = (uint8_t)(p - 0x01);       // flash, then on
        else if (p >= 0x06 && p <= 0x09) o->player = (uint8_t)(p - 0x05);  // on
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------
static uint32_t s_gp;
static uint16_t s_stick_l[2], s_stick_r[2];
static uint8_t s_lt, s_rt;
static procon_status_t s_status;
static bool s_was_mounted;
static uint32_t s_next_report;
static uint8_t s_motor_l, s_motor_r;
static uint32_t s_rumble_refresh;

#define RUMBLE_REFRESH_MS 40

void x360_init(void) {
    memset(&s_status, 0, sizeof s_status);
    s_stick_l[0] = s_stick_l[1] = s_stick_r[0] = s_stick_r[1] = S1_STICK_CENTER;
}

void x360_set_input(const s2_input_t *in, const mapping_ctx_t *ctx, uint32_t gp, bool connected) {
    s_gp = 0;
    s_lt = s_rt = 0;
    s_stick_l[0] = s_stick_l[1] = s_stick_r[0] = s_stick_r[1] = S1_STICK_CENTER;
    if (!in || !connected) return;
    s_gp = gp;
    procon_input_t tmp;
    mapping_apply(&g_settings, ctx, in, &tmp);
    memcpy(s_stick_l, tmp.stick_l, sizeof s_stick_l);
    memcpy(s_stick_r, tmp.stick_r, sizeof s_stick_r);
    if (ctx && ctx->is_gamecube) {
        int l = (int)in->trigger_l - ctx->gc_trigger_neutral[0];
        int r = (int)in->trigger_r - ctx->gc_trigger_neutral[1];
        int span_l = 255 - ctx->gc_trigger_neutral[0], span_r = 255 - ctx->gc_trigger_neutral[1];
        s_lt = (uint8_t)(l <= 0 ? 0 : l * 255 / (span_l > 0 ? span_l : 255));
        s_rt = (uint8_t)(r <= 0 ? 0 : r * 255 / (span_r > 0 ? span_r : 255));
    }
}

static void send_rumble(void) {
    rumble_sample_t l, r;
    rumble_from_motors(s_motor_l, s_motor_r, &l, &r);
    procon_hook_rumble(&l, 1, &r, 1);
    s_status.rumble_frames++;
}

void x360_on_output(const uint8_t *buf, uint16_t len) {
    x360_output_t o;
    if (!x360_parse_output(buf, len, &o)) return;
    s_status.handshake_done = true;
    if (o.rumble && (o.motor_left != s_motor_l || o.motor_right != s_motor_r)) {
        s_motor_l = o.motor_left;
        s_motor_r = o.motor_right;
        send_rumble();
        s_rumble_refresh = platform_deadline_ms(RUMBLE_REFRESH_MS);
    }
    if (o.led && o.player && o.player != s_status.player_lights) {
        s_status.player_lights = o.player;
        procon_hook_player_lights((uint8_t)((1u << o.player) - 1));
    }
}

void x360_task(void) {
    bool mounted = usb_hid_mounted();
    if (mounted != s_was_mounted) {
        s_was_mounted = mounted;
        s_motor_l = s_motor_r = 0;
        s_status.handshake_done = false;
        LOG("x360: USB %s", mounted ? "mounted" : "unmounted");
    }
    if ((s_motor_l || s_motor_r) && platform_time_reached(s_rumble_refresh)) {
        send_rumble();
        s_rumble_refresh = platform_deadline_ms(RUMBLE_REFRESH_MS);
    }
    if (!mounted || !usb_hid_ready() || !platform_time_reached(s_next_report)) return;
    uint8_t interval = g_settings.usb_report_interval_ms ? g_settings.usb_report_interval_ms : 4;
    s_next_report = platform_deadline_ms(interval);
    uint8_t r[X360_INPUT_LEN];
    x360_build_input(s_gp, s_stick_l, s_stick_r, s_lt, s_rt, r);
    // r[0] (message type 0) goes out as the "report ID" byte.
    if (usb_hid_send(r[0], r + 1, X360_INPUT_LEN - 1)) {
        s_status.reports_sent++;
    }
}

void x360_get_status(procon_status_t *out) {
    *out = s_status;
    out->usb_mounted = usb_hid_mounted();
    out->report_mode = 0x00;
    out->vibration_enabled = true;
    out->imu_enabled = false;
}
