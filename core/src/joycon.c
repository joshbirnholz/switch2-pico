#include "joycon.h"

#include <string.h>

static uint16_t clamp12(int v) {
    return (uint16_t)(v < 0 ? 0 : v > 4095 ? 4095 : v);
}

// Quarter turn left (an (L) held sideways, rail up): its right is now up and
// its down is now right. Quarter turn right (an (R)): its left is up, its up
// is right. Raw values turn around the calibrated center; the calibration's
// ranges follow their directions.
void joycon_rotate_stick(bool left, const uint16_t raw[2], const s2_stick_cal_t *cal, uint16_t out[2],
                         s2_stick_cal_t *out_cal) {
    s2_stick_cal_t c = *cal;
    if (left) {
        // new x = -old y, new y = old x
        out[0] = clamp12(2 * (int)cal->center[1] - (int)raw[1]);
        out[1] = raw[0];
        c.center[0] = cal->center[1];
        c.max[0] = cal->min[1];
        c.min[0] = cal->max[1];
        c.center[1] = cal->center[0];
        c.max[1] = cal->max[0];
        c.min[1] = cal->min[0];
    } else {
        // new x = old y, new y = -old x
        out[0] = raw[1];
        out[1] = clamp12(2 * (int)cal->center[0] - (int)raw[0]);
        c.center[0] = cal->center[1];
        c.max[0] = cal->max[1];
        c.min[0] = cal->min[1];
        c.center[1] = cal->center[0];
        c.max[1] = cal->min[0];
        c.min[1] = cal->max[0];
    }
    *out_cal = c;
}

// SL / SR of either side as GL / GR (the inputs the maps have for them).
static uint32_t side_buttons(uint32_t b) {
    if (b & (S2_BTN_SL_L | S2_BTN_SL_R)) b |= S2_BTN_GL;
    if (b & (S2_BTN_SR_L | S2_BTN_SR_R)) b |= S2_BTN_GR;
    return b;
}

void joycon_merge(ctrl_type_t type, const joycon_side_t *l, const joycon_side_t *r, s2_input_t *out,
                  mapping_ctx_t *ctx) {
    bool hl = l && l->present, hr = r && r->present;
    if (type == CTRL_JOYCON_L) hr = false;
    if (type == CTRL_JOYCON_R) hl = false;
    const joycon_side_t *imu = hr ? r : hl ? l : NULL;
    memset(out, 0, sizeof *out);
    if (imu) {
        // Motion, counters and the rest from one side.
        *out = imu->in;
        ctx->gyro_lsb_per_dps = imu->gyro_lsb_per_dps;
    }
    // Battery: the lower of the two.
    if (hl && hr && l->in.battery_mv && l->in.battery_mv < r->in.battery_mv) {
        out->battery_mv = l->in.battery_mv;
        out->charge_state = l->in.charge_state;
    }
    uint32_t b = (hl ? l->in.buttons & S2_BTNS_JOYCON_L : 0) | (hr ? r->in.buttons & S2_BTNS_JOYCON_R : 0);
    out->buttons = side_buttons(b);
    out->trigger_l = out->trigger_r = 0;

    s2_stick_cal_t def;
    s2_default_stick_cal(&def);
    ctx->cal_l = ctx->cal_r = def;
    memcpy(out->stick_l, def.center, sizeof out->stick_l);
    memcpy(out->stick_r, def.center, sizeof out->stick_r);
    if (type == CTRL_JOYCON_L || type == CTRL_JOYCON_R) {
        // Sideways: its stick, turned, is the left stick.
        const joycon_side_t *s = type == CTRL_JOYCON_L ? l : r;
        if (s && s->present) {
            const uint16_t *raw = type == CTRL_JOYCON_L ? s->in.stick_l : s->in.stick_r;
            joycon_rotate_stick(type == CTRL_JOYCON_L, raw, &s->cal, out->stick_l, &ctx->cal_l);
        }
        return;
    }
    if (hl) {
        memcpy(out->stick_l, l->in.stick_l, sizeof out->stick_l);
        ctx->cal_l = l->cal;
    }
    if (hr) {
        memcpy(out->stick_r, r->in.stick_r, sizeof out->stick_r);
        ctx->cal_r = r->cal;
    }
}

void joycon_mouse_reset(joycon_mouse_track_t *t) {
    memset(t, 0, sizeof *t);
}

void joycon_mouse_delta(joycon_mouse_track_t *t, uint16_t x, uint16_t y, int32_t *dx, int32_t *dy) {
    if (!t->valid) {
        *dx = *dy = 0;
    } else {
        // 16-bit wrap: the difference as a signed 16-bit value.
        *dx = (int16_t)(uint16_t)(x - t->x);
        *dy = (int16_t)(uint16_t)(y - t->y);
    }
    t->x = x;
    t->y = y;
    t->valid = true;
}

bool joycon_mouse_buttons(ctrl_type_t type, uint8_t src, joycon_mouse_buttons_t *out) {
    if (!ctrl_is_joycon(type) || src == MOUSE_OFF) return false;
    if (src == MOUSE_JOYCON_L) {
        *out = (joycon_mouse_buttons_t){S2_BTN_L, S2_BTN_ZL, S2_BTN_LSTICK, true};
    } else {
        *out = (joycon_mouse_buttons_t){S2_BTN_R, S2_BTN_ZR, S2_BTN_RSTICK, false};
    }
    return true;
}

void joycon_mouse_apply(const profile_t *p, int32_t dx, int32_t dy, int32_t *ox, int32_t *oy) {
    if (p->mouse_flags & MOUSE_SWAP_XY) {
        int32_t t = dx;
        dx = dy;
        dy = t;
    }
    if (p->mouse_flags & MOUSE_INVERT_X) dx = -dx;
    if (p->mouse_flags & MOUSE_INVERT_Y) dy = -dy;
    int32_t sp = p->mouse_speed_pct ? p->mouse_speed_pct : 100;
    *ox = dx * sp;
    *oy = dy * sp;
}
