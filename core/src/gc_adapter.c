#include "gc_adapter.h"

#include <string.h>

#include "hd_rumble.h"
#include "log.h"
#include "platform.h"
#include "settings.h"
#include "usb_hid.h"

// Report layouts as handled by Dolphin's GC adapter code and the Switch.
// Report descriptor in the shape of the real adapter's (214 bytes): vendor
// byte arrays per report ID, input 0x21 (36 bytes) / 0x22 / 0x23 / 0x24 /
// 0x25, output 0x11 (rumble, 4 bytes) / 0x12 / 0x13 (start) / 0x14 / 0x15.
#define COLL(id, kind, n) 0xA1, 0x01, 0x85, id, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x15, 0x00, 0x26, 0xFF, 0x00, \
                          0x75, 0x08, 0x95, n, kind, 0x00, 0xC0
#define OUT 0x91
#define IN  0x81
static const uint8_t REPORT_DESC[] = {
    0x05, 0x05, 0x09, 0x00,   // usage page: game controls
    COLL(0x11, OUT, 4), COLL(0x21, IN, 36), COLL(0x12, OUT, 1), COLL(0x22, IN, 25),
    COLL(0x13, OUT, 1), COLL(0x23, IN, 2), COLL(0x14, OUT, 1), COLL(0x24, IN, 2),
    COLL(0x15, OUT, 1), COLL(0x25, IN, 2),
};
#undef COLL

const uint8_t *gc_adapter_report_descriptor(uint16_t *len) {
    *len = sizeof REPORT_DESC;
    return REPORT_DESC;
}

// Port status: 0x10 wired controller present, 0x04 the adapter's rumble
// power cable is plugged in (hosts only rumble with it).
#define ST_WIRED   0x10
#define ST_RUMBLE  0x04
#define GC_RANGE   100   // a real stick reaches about 128 +- 100

uint8_t gc_axis(uint16_t v12) {
    long v = 128 + ((long)v12 - S1_STICK_CENTER) * GC_RANGE / S1_STICK_RANGE;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void gc_build_report(const gc_port_t *p, uint8_t r[GC_REPORT_LEN]) {
    memset(r, 0, GC_REPORT_LEN);
    r[0] = 0x21;
    for (int port = 0; port < 4; port++) r[1 + port * 9] = ST_RUMBLE;   // empty ports
    if (!p || !p->connected) return;
    uint8_t *q = r + 1;
    uint32_t gp = p->gp;
#define ON(g, bit) ((gp & GP_BIT(g)) ? (bit) : 0)
    q[0] = ST_WIRED | ST_RUMBLE;
    q[1] = (uint8_t)(ON(GP_SOUTH, 0x01) | ON(GP_WEST, 0x02) | ON(GP_EAST, 0x04) | ON(GP_NORTH, 0x08) |
                     ON(GP_LEFT, 0x10) | ON(GP_RIGHT, 0x20) | ON(GP_DOWN, 0x40) | ON(GP_UP, 0x80));
    q[2] = (uint8_t)(ON(GP_START, 0x01) | ON(GP_R1, 0x02) | ON(GP_R2, 0x04) | ON(GP_L2, 0x08));
#undef ON
    q[3] = p->stick[0];
    q[4] = p->stick[1];
    q[5] = p->cstick[0];
    q[6] = p->cstick[1];
    q[7] = p->l;
    q[8] = p->r;
}

bool gc_parse_output(const uint8_t *buf, uint16_t len, gc_output_t *o) {
    memset(o, 0, sizeof *o);
    if (len < 1) return false;
    if (buf[0] == 0x13) {
        o->start = true;
        return true;
    }
    if (buf[0] == 0x11 && len >= 2) {
        o->rumble = true;
        o->rumble_on = (buf[1] & 0x03) == 1;   // 1 = on; 0 = off, 2 = brake
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------
static gc_port_t s_port;
static procon_status_t s_status;
static bool s_was_mounted;
static uint32_t s_next_report;
static bool s_rumble;
static uint32_t s_rumble_refresh;

#define RUMBLE_REFRESH_MS 40

void gc_adapter_init(void) {
    memset(&s_port, 0, sizeof s_port);
    memset(&s_status, 0, sizeof s_status);
}

void gc_adapter_set_input(const s2_input_t *in, const mapping_ctx_t *ctx, uint32_t gp, bool connected) {
    memset(&s_port, 0, sizeof s_port);
    s_port.stick[0] = s_port.stick[1] = s_port.cstick[0] = s_port.cstick[1] = 128;
    if (!in || !connected) return;
    s_port.connected = true;
    s_port.gp = gp;
    procon_input_t tmp;
    mapping_apply(&g_settings, ctx, in, &tmp);
    s_port.stick[0] = gc_axis(tmp.stick_l[0]);
    s_port.stick[1] = gc_axis(tmp.stick_l[1]);
    s_port.cstick[0] = gc_axis(tmp.stick_r[0]);
    s_port.cstick[1] = gc_axis(tmp.stick_r[1]);
    if (ctx && ctx->is_gamecube) {
        // Real analog triggers: from the controller's resting value to full.
        int l = (int)in->trigger_l - ctx->gc_trigger_neutral[0];
        int r = (int)in->trigger_r - ctx->gc_trigger_neutral[1];
        int span_l = 255 - ctx->gc_trigger_neutral[0], span_r = 255 - ctx->gc_trigger_neutral[1];
        s_port.l = (uint8_t)(l <= 0 ? 0 : l * 255 / (span_l > 0 ? span_l : 255));
        s_port.r = (uint8_t)(r <= 0 ? 0 : r * 255 / (span_r > 0 ? span_r : 255));
    } else {
        // Digital shoulders: a full press, like a GameCube trigger clicked in.
        if (gp & GP_BIT(GP_L2)) s_port.l = 255;
        if (gp & GP_BIT(GP_R2)) s_port.r = 255;
    }
}

static void send_rumble(void) {
    // One motor in the middle of the controller: the strong one, both sides.
    rumble_sample_t l, r;
    rumble_from_motors(s_rumble ? 255 : 0, 0, &l, &r);
    procon_hook_rumble(&l, 1, &l, 1);
    s_status.rumble_frames++;
}

void gc_adapter_on_output(const uint8_t *buf, uint16_t len) {
    gc_output_t o;
    if (!gc_parse_output(buf, len, &o)) return;
    if (o.start && !s_status.handshake_done) {
        s_status.handshake_done = true;
        LOG("gc: host started the adapter");
    }
    if (o.rumble && o.rumble_on != s_rumble) {
        s_rumble = o.rumble_on;
        send_rumble();
        s_rumble_refresh = platform_deadline_ms(RUMBLE_REFRESH_MS);
    }
}

void gc_adapter_task(void) {
    bool mounted = usb_hid_mounted();
    if (mounted != s_was_mounted) {
        s_was_mounted = mounted;
        s_rumble = false;
        s_status.handshake_done = false;
        LOG("gc: USB %s", mounted ? "mounted" : "unmounted");
    }
    if (s_rumble && platform_time_reached(s_rumble_refresh)) {
        send_rumble();
        s_rumble_refresh = platform_deadline_ms(RUMBLE_REFRESH_MS);
    }
    if (!mounted || !usb_hid_ready() || !platform_time_reached(s_next_report)) return;
    uint8_t interval = g_settings.usb_report_interval_ms ? g_settings.usb_report_interval_ms : 8;
    s_next_report = platform_deadline_ms(interval);
    uint8_t r[GC_REPORT_LEN];
    gc_build_report(&s_port, r);
    if (usb_hid_send(r[0], r + 1, GC_REPORT_LEN - 1)) s_status.reports_sent++;
}

void gc_adapter_get_status(procon_status_t *out) {
    *out = s_status;
    out->usb_mounted = usb_hid_mounted();
    out->report_mode = 0x21;
    out->vibration_enabled = true;
    out->imu_enabled = false;
}
