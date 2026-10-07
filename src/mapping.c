#include "mapping.h"

#include <math.h>

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
};

uint32_t mapping_out_button_bit(out_button_t b) {
    return b < OUT_COUNT ? OUT_BITS[b] : 0;
}

uint32_t mapping_in_button_s2_bit(in_button_t b) {
    return b < IN_COUNT ? IN_BITS[b] : 0;
}

uint32_t mapping_buttons(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in) {
    uint32_t raw = in->buttons;
    if (ctx && ctx->is_gamecube) {
        // Analog GameCube triggers count as the shoulder press past the threshold.
        int l = (int)in->trigger_l - ctx->gc_trigger_neutral[0];
        int r = (int)in->trigger_r - ctx->gc_trigger_neutral[1];
        if (l > s->gc_trigger_threshold) raw |= S2_BTN_L;
        if (r > s->gc_trigger_threshold) raw |= S2_BTN_R;
    }
    uint32_t out = 0;
    for (int i = 0; i < IN_COUNT; i++) {
        if (raw & IN_BITS[i]) {
            out |= mapping_out_button_bit((out_button_t)s->button_map[i]);
        }
    }
    return out;
}

void mapping_stick(const settings_t *s, const s2_stick_cal_t *cal, const uint16_t raw[2], uint16_t out[2]) {
    float x = s2_stick_axis(cal, 0, raw[0]);
    float y = s2_stick_axis(cal, 1, raw[1]);
    float mag = sqrtf(x * x + y * y);
    float dz = (float)s->stick_deadzone_pct / 100.0f;
    float outer = (float)s->stick_outer_pct / 100.0f;
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

void mapping_apply(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in, procon_input_t *out) {
    out->buttons = mapping_buttons(s, ctx, in);
    const uint16_t *lraw = s->swap_sticks ? in->stick_r : in->stick_l;
    const uint16_t *rraw = s->swap_sticks ? in->stick_l : in->stick_r;
    const s2_stick_cal_t *lcal = s->swap_sticks ? &ctx->cal_r : &ctx->cal_l;
    const s2_stick_cal_t *rcal = s->swap_sticks ? &ctx->cal_l : &ctx->cal_r;
    mapping_stick(s, lcal, lraw, out->stick_l);
    mapping_stick(s, rcal, rraw, out->stick_r);
    mapping_imu(s, ctx, in, out->accel, out->gyro);
}

void mapping_pack_stick(const uint16_t v[2], uint8_t out[3]) {
    out[0] = (uint8_t)(v[0] & 0xFF);
    out[1] = (uint8_t)(((v[0] >> 8) & 0x0F) | ((v[1] & 0x0F) << 4));
    out[2] = (uint8_t)(v[1] >> 4);
}
