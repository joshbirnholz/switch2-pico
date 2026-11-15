#include "joycon.h"

#include <math.h>
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

// SL / SR as the inputs the maps have for them: on a pair the (L)'s are GL
// / GR and the (R)'s are IN_SL_R / IN_SR_R (their own bits); a single
// Joy-Con 2's are GL / GR.
static uint32_t side_buttons(uint32_t b, bool pair) {
    if (pair) {
        if (b & S2_BTN_SL_L) b |= S2_BTN_GL;
        if (b & S2_BTN_SR_L) b |= S2_BTN_GR;
        return b;
    }
    if (b & (S2_BTN_SL_L | S2_BTN_SL_R)) b |= S2_BTN_GL;
    if (b & (S2_BTN_SR_L | S2_BTN_SR_R)) b |= S2_BTN_GR;
    return b & ~(S2_BTN_SL_R | S2_BTN_SR_R);
}

void joycon_merge(ctrl_type_t type, joycon_gyro_side_t gyro_side, const joycon_side_t *l, const joycon_side_t *r, s2_input_t *out,
                  mapping_ctx_t *ctx) {
    bool hl = l && l->present, hr = r && r->present;
    if (type == CTRL_JOYCON_L) hr = false;
    if (type == CTRL_JOYCON_R) hl = false;
    const joycon_side_t *imu = gyro_side == JOYCON_GYRO_LEFT && hl ? l : hr ? r : hl ? l : NULL;
    memset(out, 0, sizeof *out);
    if (imu) {
        // Motion, counters and the rest from one side.
        *out = imu->in;
        ctx->gyro_lsb_per_dps = imu->gyro_lsb_per_dps;
    }
    // Battery: the lower of the two.
    const joycon_side_t *battery = hr ? r : hl ? l : NULL;
    if (hl && hr && l->in.battery_mv && (!r->in.battery_mv || l->in.battery_mv < r->in.battery_mv)) battery = l;
    if (battery) { out->battery_mv = battery->in.battery_mv; out->charge_state = battery->in.charge_state; }
    uint32_t b = (hl ? l->in.buttons & S2_BTNS_JOYCON_L : 0) | (hr ? r->in.buttons & S2_BTNS_JOYCON_R : 0);
    out->buttons = side_buttons(b, type == CTRL_JOYCON_PAIR);
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

void joycon_mouse_buttons(ctrl_type_t type, bool left, joycon_mouse_buttons_t *out) {
    if (left) *out = (joycon_mouse_buttons_t){S2_BTN_L, S2_BTN_ZL, S2_BTN_LSTICK, true};
    else *out = (joycon_mouse_buttons_t){S2_BTN_R, S2_BTN_ZR, S2_BTN_RSTICK, false};
    // A single (sideways) Joy-Con 2's stick is the left stick.
    if (type != CTRL_JOYCON_PAIR) out->stick_left = true;
}

bool joycon_mouse_pose(const int16_t accel[3], bool was) {
    float x = accel[0], y = accel[1], z = accel[2];
    float g2 = x * x + y * y + z * z;
    const float one_g = 32767.0f / 8.0f;   // +-8 g full scale
    if (g2 < 0.36f * one_g * one_g || g2 > 1.96f * one_g * one_g) return was;   // 0.6..1.4 g
    // cos^2 of the angle between gravity and the X axis.
    float c2 = x * x / g2;
    return was ? c2 > 0.179f    // cos^2 65 degrees
               : c2 > 0.329f;   // cos^2 55 degrees
}

void joycon_mouse_scroll(bool left, float x, float y, uint8_t flags, float *wheel, float *pan) {
    *wheel = *pan = 0.0f;
    // Signs as checked on hardware: sideways, both Joy-Cons the same way;
    // left / right scrolling up / down, the (R) the other way round.
    bool up_down = flags & MOUSE_FLAG_SCROLL_UP_DOWN_ONLY;
    float h = up_down ? (left ? -x : x) : x;
    if (flags & MOUSE_FLAG_INVERT_UP_DOWN) y = -y;
    if (flags & MOUSE_FLAG_INVERT_LEFT_RIGHT) h = -h;
    if (fabsf(y) >= fabsf(x)) *wheel = y;
    else if (up_down) *wheel = h;
    else *pan = h;
}

void joycon_mouse_apply(const profile_t *p, int32_t dx, int32_t dy, int32_t *ox, int32_t *oy) {
    int32_t sp = p->mouse_speed_pct ? p->mouse_speed_pct : 100;
    *ox = dx * sp;
    *oy = dy * sp;
}

// ---------------------------------------------------------------------------
// Gyro mouse
// ---------------------------------------------------------------------------
int joycon_gyro_mouse_side(ctrl_type_t type, joycon_gyro_side_t gyro_side, bool has_l, bool has_r) {
    if (type == CTRL_JOYCON_L) has_r = false;
    if (type == CTRL_JOYCON_R) has_l = false;
    if (gyro_side == JOYCON_GYRO_LEFT && has_l) return 0;
    if (has_r) return 1;
    if (has_l) return 0;
    return -1;
}

uint32_t joycon_combo_raw(uint32_t in_bits) {
    uint32_t raw = 0;
    for (int i = 0; i < IN_COUNT; i++) {
        if (in_bits & (1u << i)) raw |= mapping_in_button_s2_bit((in_button_t)i);
    }
    return raw;
}

void joycon_gyro_mouse_reset(joycon_gyro_mouse_t *g) {
    memset(g, 0, sizeof *g);
}

#define GYRO_MOUSE_UP_TAU    0.3f   // s: how quickly "up" follows the accelerometer
#define GYRO_MOUSE_TIGHTEN   1.0f   // deg/s: slower turns are scaled down (tremor)

static float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static bool normalize3(float v[3]) {
    float n = sqrtf(dot3(v, v));
    if (n < 1e-6f) return false;
    for (int k = 0; k < 3; k++) v[k] /= n;
    return true;
}

void joycon_gyro_mouse_step(joycon_gyro_mouse_t *g, ctrl_type_t type, int side, const profile_t *p,
                            const float gyro_dps[3], const int16_t accel[3], float dt, float *dx, float *dy) {
    *dx = *dy = 0.0f;
    // Up, from gravity: only readings near 1 g count (not while shaken).
    float a[3] = {accel[0], accel[1], accel[2]};
    const float one_g = 32767.0f / 8.0f;
    float g2 = dot3(a, a);
    if (g2 > 0.49f * one_g * one_g && g2 < 1.69f * one_g * one_g && normalize3(a)) {
        if (!g->have_up) {
            memcpy(g->up, a, sizeof g->up);
            g->have_up = true;
        } else {
            float k = dt / (GYRO_MOUSE_UP_TAU + dt);
            for (int i = 0; i < 3; i++) g->up[i] += (a[i] - g->up[i]) * k;
            if (!normalize3(g->up)) memcpy(g->up, a, sizeof g->up);
        }
    }
    if (!g->have_up || dt <= 0.0f) return;
    // Yaw: the turn around the vertical. Pitch: around the axis from the
    // hand's left to its right, made level: the Joy-Con's width, or its
    // length on a single one held sideways (an (L)'s top points left, an
    // (R)'s right).
    float yaw = dot3(gyro_dps, g->up);
    float right[3] = {1.0f, 0.0f, 0.0f};
    if (type == CTRL_JOYCON_L || type == CTRL_JOYCON_R) {
        right[0] = 0.0f;
        right[1] = side == 0 ? -1.0f : 1.0f;
    }
    float along = dot3(right, g->up);
    for (int i = 0; i < 3; i++) right[i] -= along * g->up[i];
    float pitch = normalize3(right) ? dot3(gyro_dps, right) : 0.0f;
    // Counter-clockwise from above (positive yaw) is a turn to the left; a
    // positive pitch tilts the top up: pointer left, pointer up.
    float x = -yaw, y = -pitch;
    float speed = sqrtf(x * x + y * y);
    if (speed < GYRO_MOUSE_TIGHTEN) {
        x *= speed / GYRO_MOUSE_TIGHTEN;
        y *= speed / GYRO_MOUSE_TIGHTEN;
    }
    float scale = GYRO_MOUSE_COUNTS_PER_DEG * (float)(p->gyro_mouse_speed_pct ? p->gyro_mouse_speed_pct : 100) / 100.0f * dt;
    if (p->gyro_mouse_flags & GYRO_MOUSE_FLAG_INVERT_X) x = -x;
    if (p->gyro_mouse_flags & GYRO_MOUSE_FLAG_INVERT_Y) y = -y;
    *dx = x * scale;
    *dy = y * scale;
}
