#include "mapping.h"

#include <math.h>
#include <string.h>

// Macros map to 0 here; see mapping_macro_step().
static const uint32_t OUT_BITS[OUT_COUNT] = {
    [OUT_NONE] = 0,
    [OUT_A] = S1_BTN_A, [OUT_B] = S1_BTN_B, [OUT_X] = S1_BTN_X, [OUT_Y] = S1_BTN_Y,
    [OUT_L] = S1_BTN_L, [OUT_R] = S1_BTN_R, [OUT_ZL] = S1_BTN_ZL, [OUT_ZR] = S1_BTN_ZR,
    [OUT_MINUS] = S1_BTN_MINUS, [OUT_PLUS] = S1_BTN_PLUS,
    [OUT_LSTICK] = S1_BTN_LSTICK, [OUT_RSTICK] = S1_BTN_RSTICK,
    [OUT_HOME] = S1_BTN_HOME, [OUT_CAPTURE] = S1_BTN_CAPTURE,
    [OUT_UP] = S1_BTN_UP, [OUT_DOWN] = S1_BTN_DOWN, [OUT_LEFT] = S1_BTN_LEFT, [OUT_RIGHT] = S1_BTN_RIGHT,
};

static const uint32_t IN_BITS[IN_COUNT] = {
    [IN_A] = S2_BTN_A, [IN_B] = S2_BTN_B, [IN_X] = S2_BTN_X, [IN_Y] = S2_BTN_Y,
    [IN_L] = S2_BTN_L, [IN_R] = S2_BTN_R, [IN_ZL] = S2_BTN_ZL, [IN_ZR] = S2_BTN_ZR,
    [IN_MINUS] = S2_BTN_MINUS, [IN_PLUS] = S2_BTN_PLUS,
    [IN_LSTICK] = S2_BTN_LSTICK, [IN_RSTICK] = S2_BTN_RSTICK,
    [IN_HOME] = S2_BTN_HOME, [IN_CAPTURE] = S2_BTN_CAPTURE,
    [IN_UP] = S2_BTN_UP, [IN_DOWN] = S2_BTN_DOWN, [IN_LEFT] = S2_BTN_LEFT, [IN_RIGHT] = S2_BTN_RIGHT,
    [IN_GL] = S2_BTN_GL, [IN_GR] = S2_BTN_GR, [IN_C] = S2_BTN_C,
    [IN_SL_R] = S2_BTN_SL_R, [IN_SR_R] = S2_BTN_SR_R,
};

// A Joy-Con 2 on its own is held sideways, rail up: the four buttons under
// the thumb act as A / B / X / Y by where they end up, SL / SR are the
// shoulders (they arrive as GL / GR, see joycon.c), and its stick is the
// left stick. Indexed by the Nintendo label each button takes:
// {A, B, X, Y} = {east, south, north, west} once rotated.
static const in_button_t SIDE_FACE_L[4] = {IN_DOWN, IN_LEFT, IN_RIGHT, IN_UP};   // rotated a quarter left
static const in_button_t SIDE_FACE_R[4] = {IN_X, IN_A, IN_Y, IN_B};              // rotated a quarter right

// Declared in settings.h (settings_defaults() uses them).
void settings_default_button_map(ctrl_type_t type, uint8_t map[IN_COUNT]) {
    // The GameCube controller's buttons arrive as their Switch equivalents.
    if (type == CTRL_JOYCON_L || type == CTRL_JOYCON_R) {
        bool l = type == CTRL_JOYCON_L;
        const in_button_t *face = l ? SIDE_FACE_L : SIDE_FACE_R;
        memset(map, OUT_NONE, IN_COUNT);
        map[face[0]] = OUT_A;
        map[face[1]] = OUT_B;
        map[face[2]] = OUT_X;
        map[face[3]] = OUT_Y;
        map[IN_GL] = OUT_L;   // SL
        map[IN_GR] = OUT_R;   // SR
        map[l ? IN_L : IN_R] = OUT_ZL;
        map[l ? IN_ZL : IN_ZR] = OUT_ZR;
        map[l ? IN_LSTICK : IN_RSTICK] = OUT_LSTICK;
        if (l) {
            map[IN_MINUS] = OUT_PLUS;
            map[IN_CAPTURE] = OUT_HOME;
        } else {
            map[IN_PLUS] = OUT_PLUS;
            map[IN_HOME] = OUT_HOME;
            map[IN_C] = OUT_MINUS;
        }
        return;
    }
    // Identity for every button the Switch 1 Pro Controller also has; GL / GR
    // default to the stick clicks (like most back paddle setups), C unassigned.
    // A Joy-Con 2 pair: SL / SR (as GL / GR) unassigned.
    for (int i = IN_A; i <= IN_RIGHT; i++) map[i] = (uint8_t)(OUT_A + (i - IN_A));
    bool pair = type == CTRL_JOYCON_PAIR;
    map[IN_GL] = pair ? OUT_NONE : OUT_LSTICK;
    map[IN_GR] = pair ? OUT_NONE : OUT_RSTICK;
    map[IN_C] = OUT_NONE;
    map[IN_SL_R] = map[IN_SR_R] = OUT_NONE;
}

// Sideways Joy-Con 2, non-Switch modes (positional outputs).
static void side_mode_map(bool l, usb_mode_t mode, uint8_t map[IN_COUNT]) {
    const in_button_t *face = l ? SIDE_FACE_L : SIDE_FACE_R;
    memset(map, GP_NONE, IN_COUNT);
    if (mode == USB_MODE_GC_ADAPTER) {
        // By label, as for the other controllers.
        map[face[0]] = GP_SOUTH;
        map[face[1]] = GP_WEST;
        map[face[2]] = GP_EAST;
        map[face[3]] = GP_NORTH;
        map[IN_GL] = GP_L2;
        map[IN_GR] = GP_R2;
        map[l ? IN_ZL : IN_ZR] = GP_R1;   // Z
        map[l ? IN_MINUS : IN_PLUS] = GP_START;
        return;
    }
    map[face[0]] = GP_EAST;
    map[face[1]] = GP_SOUTH;
    map[face[2]] = GP_NORTH;
    map[face[3]] = GP_WEST;
    map[IN_GL] = GP_L1;
    map[IN_GR] = GP_R1;
    map[l ? IN_L : IN_R] = GP_L2;
    map[l ? IN_ZL : IN_ZR] = GP_R2;
    map[l ? IN_LSTICK : IN_RSTICK] = GP_L3;
    if (l) {
        map[IN_MINUS] = GP_START;
        map[IN_CAPTURE] = GP_GUIDE;
    } else {
        map[IN_PLUS] = GP_START;
        map[IN_HOME] = GP_GUIDE;
        map[IN_C] = GP_SELECT;
    }
}

void settings_default_mode_map(ctrl_type_t type, usb_mode_t mode, uint8_t map[IN_COUNT]) {
    static const uint8_t base[IN_COUNT] = {
        // Nintendo letters by position: A right, B bottom, X top, Y left.
        [IN_A] = GP_EAST, [IN_B] = GP_SOUTH, [IN_X] = GP_NORTH, [IN_Y] = GP_WEST,
        [IN_L] = GP_L1, [IN_R] = GP_R1, [IN_ZL] = GP_L2, [IN_ZR] = GP_R2,
        [IN_MINUS] = GP_SELECT, [IN_PLUS] = GP_START, [IN_LSTICK] = GP_L3, [IN_RSTICK] = GP_R3,
        [IN_HOME] = GP_GUIDE, [IN_CAPTURE] = GP_NONE,
        [IN_UP] = GP_UP, [IN_DOWN] = GP_DOWN, [IN_LEFT] = GP_LEFT, [IN_RIGHT] = GP_RIGHT,
        [IN_GL] = GP_NONE, [IN_GR] = GP_NONE, [IN_C] = GP_NONE,
    };
    if (type == CTRL_JOYCON_L || type == CTRL_JOYCON_R) {
        side_mode_map(type == CTRL_JOYCON_L, mode, map);
        return;
    }
    memcpy(map, base, IN_COUNT);
    bool gc = type == CTRL_GAMECUBE;
    if (gc) {
        // GameCube controller: its analog L / R are the triggers, Z (its ZR)
        // and its ZL the bumpers.
        map[IN_L] = GP_L2;
        map[IN_R] = GP_R2;
        map[IN_ZL] = GP_L1;
        map[IN_ZR] = GP_R1;
    }
    switch (mode) {
    case USB_MODE_DUALSENSE_EDGE:
        map[IN_CAPTURE] = GP_TOUCHPAD;
        map[IN_GL] = GP_PADDLE_L;
        map[IN_GR] = GP_PADDLE_R;
        map[IN_C] = GP_FN_R;
        break;
    case USB_MODE_DUALSENSE:
        map[IN_CAPTURE] = GP_TOUCHPAD;
        break;
    case USB_MODE_GC_ADAPTER:
        // By label; buttons a GameCube controller lacks are unassigned.
        memset(map, GP_NONE, IN_COUNT);
        map[IN_A] = GP_SOUTH;
        map[IN_B] = GP_WEST;
        map[IN_X] = GP_EAST;
        map[IN_Y] = GP_NORTH;
        map[IN_PLUS] = GP_START;
        map[IN_UP] = GP_UP;
        map[IN_DOWN] = GP_DOWN;
        map[IN_LEFT] = GP_LEFT;
        map[IN_RIGHT] = GP_RIGHT;
        if (gc) {
            map[IN_L] = GP_L2;    // analog L / R
            map[IN_R] = GP_R2;
            map[IN_ZR] = GP_R1;   // Z
        } else {
            // Pro Controller: triggers as L / R, R as Z.
            map[IN_ZL] = GP_L2;
            map[IN_ZR] = GP_R2;
            map[IN_R] = GP_R1;
        }
        break;
    default:
        break;
    }
}

void settings_default_profile(ctrl_type_t type, ctrl_profile_t *p) {
    // Legacy maps: the inputs there were then.
    uint8_t map[IN_COUNT];
    settings_default_button_map(type, map);
    memcpy(p->button_map, map, IN_COUNT_V1);
    for (int m = 0; m < MODE_MAP_SLOTS; m++) {
        settings_default_mode_map(type, (usb_mode_t)m, map);
        memcpy(p->mode_map[m], map, IN_COUNT_V1);
    }
    settings_default_mode_slots(p->mode_slot);
    p->stick_deadzone_pct = 6;
    p->stick_outer_pct = 95;
    p->swap_sticks = 0;
    p->rumble_enabled = 1;
    p->rumble_strength_pct = 100;
}

uint32_t mapping_out_button_bit(out_button_t b) {
    return b < OUT_COUNT ? OUT_BITS[b] : 0;
}

uint32_t mapping_in_button_s2_bit(in_button_t b) {
    return b < IN_COUNT ? IN_BITS[b] : 0;
}

uint32_t mapping_buttons(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in) {
    return mapping_buttons_except(s, ctx, in, 0);
}

uint32_t mapping_buttons_except(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in, uint32_t skip) {
    uint32_t raw = in->buttons & ~skip;
    if (ctx && ctx->is_gamecube) {
        // Analog GameCube triggers count as the shoulder press past the threshold.
        int l = (int)in->trigger_l - ctx->gc_trigger_neutral[0];
        int r = (int)in->trigger_r - ctx->gc_trigger_neutral[1];
        int th = settings_active(s, CTRL_GAMECUBE)->trigger_threshold;
        if (l > th) raw |= S2_BTN_L;
        if (r > th) raw |= S2_BTN_R;
    }
    uint32_t out = 0;
    for (int i = 0; i < IN_COUNT; i++) {
        if (raw & IN_BITS[i]) {
            out |= mapping_out_button_bit((out_button_t)settings_active_map(s, mapping_ctrl_type(ctx))[i]);
        }
    }
    return out;
}

static void stick_scaled(uint8_t deadzone_pct, uint8_t outer_pct, const s2_stick_cal_t *cal, const uint16_t raw[2],
                         uint16_t out[2]);

void mapping_stick(const settings_t *s, const s2_stick_cal_t *cal, const uint16_t raw[2], uint16_t out[2]) {
    ctrl_tuning_t t = settings_tuning(s, CTRL_PRO);
    stick_scaled(t.deadzone, t.outer, cal, raw, out);
}

static void stick_scaled(uint8_t deadzone_pct, uint8_t outer_pct, const s2_stick_cal_t *cal, const uint16_t raw[2],
                         uint16_t out[2]) {
    float x = s2_stick_axis(cal, 0, raw[0]);
    float y = s2_stick_axis(cal, 1, raw[1]);
    float mag = sqrtf(x * x + y * y);
    float dz = (float)deadzone_pct / 100.0f;
    float outer = (float)outer_pct / 100.0f;
    if (outer <= dz + 0.01f) outer = dz + 0.01f;
    float scaled;
    if (mag <= dz) {
        scaled = 0.0f;
    } else {
        scaled = (mag - dz) / (outer - dz);
        if (scaled > 1.0f) scaled = 1.0f;
    }
    if (mag > 0.0001f) {
        x = x / mag * scaled;
        y = y / mag * scaled;
    } else {
        x = y = 0.0f;
    }
    int ox = S1_STICK_CENTER + (int)lroundf(x * S1_STICK_RANGE);
    int oy = S1_STICK_CENTER + (int)lroundf(y * S1_STICK_RANGE);
    out[0] = (uint16_t)(ox < 0 ? 0 : (ox > 4095 ? 4095 : ox));
    out[1] = (uint16_t)(oy < 0 ? 0 : (oy > 4095 ? 4095 : oy));
}

static int16_t sat16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)lroundf(v);
}

// Axis conventions, derived from SDL's two drivers (which both convert to the
// same PlayStation-style frame):
//   Switch 2 -> SDL : (x, z, -y)            for both accel and gyro
//   Switch 1 -> SDL : (-y, z, -x)
// so Switch 1 = (S2.y, -S2.x, S2.z).
void mapping_imu(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in,
                 int16_t accel_out[3], int16_t gyro_out[3]) {
    if (!s->gyro_enabled) {
        accel_out[0] = accel_out[1] = accel_out[2] = 0;
        gyro_out[0] = gyro_out[1] = gyro_out[2] = 0;
        return;
    }
    float a_scale = (S1_ACCEL_LSB_PER_G / S2_ACCEL_LSB_PER_G) * (float)s->accel_scale_pct / 100.0f;
    float native = ctx && ctx->gyro_lsb_per_dps > 1.0f ? ctx->gyro_lsb_per_dps : S2_GYRO_LSB_PER_DPS_A;
    float g_scale = (S1_GYRO_LSB_PER_DPS / native) * (float)s->gyro_scale_pct / 100.0f;

    float ax = in->accel[0], ay = in->accel[1], az = in->accel[2];
    float gx = (float)in->gyro[0] - s->gyro_bias[0];
    float gy = (float)in->gyro[1] - s->gyro_bias[1];
    float gz = (float)in->gyro[2] - s->gyro_bias[2];

    accel_out[0] = sat16(ay * a_scale);
    accel_out[1] = sat16(-ax * a_scale);
    accel_out[2] = sat16(az * a_scale);
    gyro_out[0] = sat16(gy * g_scale);
    gyro_out[1] = sat16(-gx * g_scale);
    gyro_out[2] = sat16(gz * g_scale);
}

void mapping_imu_sdl(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in, float accel_g[3],
                     float gyro_dps[3]) {
    if (!s->gyro_enabled) {
        for (int i = 0; i < 3; i++) accel_g[i] = gyro_dps[i] = 0.0f;
        return;
    }
    float a_scale = (float)s->accel_scale_pct / (100.0f * S2_ACCEL_LSB_PER_G);
    float native = ctx && ctx->gyro_lsb_per_dps > 1.0f ? ctx->gyro_lsb_per_dps : S2_GYRO_LSB_PER_DPS_A;
    float g_scale = (float)s->gyro_scale_pct / (100.0f * native);
    float gx = (float)in->gyro[0] - s->gyro_bias[0];
    float gy = (float)in->gyro[1] - s->gyro_bias[1];
    float gz = (float)in->gyro[2] - s->gyro_bias[2];
    // Same axis mapping as SDL's Switch 2 driver.
    accel_g[0] = in->accel[0] * a_scale;
    accel_g[1] = in->accel[2] * a_scale;
    accel_g[2] = -in->accel[1] * a_scale;
    gyro_dps[0] = gx * g_scale;
    gyro_dps[1] = gz * g_scale;
    gyro_dps[2] = -gy * g_scale;
}

void mapping_apply(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in, procon_input_t *out) {
    out->buttons = mapping_buttons(s, ctx, in);
    // The connected controller type's deadzone / range / swap.
    ctrl_tuning_t t = settings_tuning(s, mapping_ctrl_type(ctx));
    const uint16_t *lraw = t.swap ? in->stick_r : in->stick_l;
    const uint16_t *rraw = t.swap ? in->stick_l : in->stick_r;
    const s2_stick_cal_t *lcal = t.swap ? &ctx->cal_r : &ctx->cal_l;
    const s2_stick_cal_t *rcal = t.swap ? &ctx->cal_l : &ctx->cal_r;
    stick_scaled(t.deadzone, t.outer, lcal, lraw, out->stick_l);
    stick_scaled(t.deadzone, t.outer, rcal, rraw, out->stick_r);
    mapping_imu(s, ctx, in, out->accel, out->gyro);
}

void mapping_pack_stick(const uint16_t v[2], uint8_t out[3]) {
    out[0] = (uint8_t)(v[0] & 0xFF);
    out[1] = (uint8_t)(((v[0] >> 8) & 0x0F) | ((v[1] & 0x0F) << 4));
    out[2] = (uint8_t)(v[1] >> 4);
}

// The inputs the C + button shortcut remaps: GL / GR (a Joy-Con 2 pair: the
// (L)'s SL / SR) and the (R)'s SL / SR.
static const in_button_t REMAP_BACK[] = {IN_GL, IN_GR, IN_SL_R, IN_SR_R};
#define REMAP_BACK_BITS (S2_BTN_GL | S2_BTN_GR | S2_BTN_SL_R | S2_BTN_SR_R)

bool mapping_quick_remap_held(uint32_t raw) {
    return (raw & S2_BTN_C) && (raw & REMAP_BACK_BITS);
}

bool mapping_quick_remap(settings_t *s, ctrl_type_t type, uint32_t prev, uint32_t raw, in_button_t *changed) {
    return mapping_quick_remap_map(settings_active_map(s, type), prev, raw, changed);
}

bool mapping_quick_remap_map(uint8_t map[IN_COUNT], uint32_t prev, uint32_t raw, in_button_t *changed) {
    if (!(raw & S2_BTN_C)) return false;
    // Exactly one of them held (none, or several: ambiguous).
    int held = 0;
    in_button_t back = IN_GL;
    for (size_t k = 0; k < sizeof REMAP_BACK / sizeof REMAP_BACK[0]; k++) {
        if (raw & IN_BITS[REMAP_BACK[k]]) {
            held++;
            back = REMAP_BACK[k];
        }
    }
    if (held != 1) return false;
    uint32_t pressed = raw & ~prev;
    for (int i = 0; i < IN_COUNT; i++) {
        // Home stays out: C + Home is the configuration Wi-Fi hotkey.
        if ((IN_BITS[i] & REMAP_BACK_BITS) || i == IN_C || i == IN_HOME) continue;
        if (!(pressed & IN_BITS[i])) continue;
        uint8_t target = map[i];
        map[back] = map[back] == target ? 0 : target;   // 0: OUT_NONE / GP_NONE
        *changed = back;
        return true;
    }
    return false;
}

bool mapping_macro_busy(const mapping_macro_t *m) {
    return m->running || m->held;
}

uint32_t mapping_macro_step(mapping_macro_t *m, const settings_t *s, ctrl_type_t type, uint32_t prev, uint32_t raw,
                            uint32_t now_ms) {
    switch (mapping_macro_run(m, settings_active_map(s, type), OUT_HOME_A, prev, raw, now_ms)) {
    case MACRO_GUIDE: return S1_BTN_HOME;
    case MACRO_GUIDE_SOUTH: return S1_BTN_HOME | S1_BTN_A;
    default: return 0;
    }
}

macro_phase_t mapping_macro_run(mapping_macro_t *m, const uint8_t map[IN_COUNT], uint8_t macro_value, uint32_t prev,
                                uint32_t raw, uint32_t now_ms) {
    uint32_t pressed = raw & ~prev;
    if (m->held) {
        if (pressed & ~m->held) m->spoiled = true;
        if (!(raw & m->held)) {
            if (!m->spoiled && !m->running) {
                m->running = true;
                m->start_ms = now_ms;
            }
            m->held = 0;
        }
    } else {
        for (int i = 0; i < IN_COUNT; i++) {
            if (map[i] == macro_value && (pressed & IN_BITS[i])) {
                m->held = IN_BITS[i];
                // Pressed together with others (e.g. already holding GL): not a tap.
                m->spoiled = (raw & ~IN_BITS[i]) != 0;
                break;
            }
        }
    }
    if (!m->running) return MACRO_IDLE;
    uint32_t t = now_ms - m->start_ms;
    if (t < MACRO_HOME_MS) return MACRO_GUIDE;
    if (t < MACRO_HOME_MS + MACRO_HOME_A_MS) return MACRO_GUIDE_SOUTH;
    m->running = false;
    return MACRO_IDLE;
}

uint32_t mapping_gp_buttons(const settings_t *s, const uint8_t map[IN_COUNT], const mapping_ctx_t *ctx,
                            const s2_input_t *in) {
    uint32_t raw = in->buttons;
    if (ctx && ctx->is_gamecube) {
        int l = (int)in->trigger_l - ctx->gc_trigger_neutral[0];
        int r = (int)in->trigger_r - ctx->gc_trigger_neutral[1];
        int th = settings_active(s, CTRL_GAMECUBE)->trigger_threshold;
        if (l > th) raw |= S2_BTN_L;
        if (r > th) raw |= S2_BTN_R;
    }
    uint32_t out = 0;
    for (int i = 0; i < IN_COUNT; i++) {
        uint8_t o = map[i];
        if ((raw & IN_BITS[i]) && o != GP_NONE && o != GP_MACRO_QAM && o < GP_COUNT) out |= GP_BIT(o);
    }
    return out;
}
