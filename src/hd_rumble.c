#include "hd_rumble.h"

#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Switch 1 HD rumble
//
// Each motor gets 4 bytes, read as a little-endian 32-bit word `w`. The top two
// bits select how many amplitude/frequency updates are packed into the word:
//
//   mode 0: no change, keep playing the current state
//   mode 1: (a) the "classic" encoding documented by dekuNukem
//               bits 2..8  high band frequency  (code + 0x60 = 32*log2(f/10))
//               bits 9..15 high band amplitude  (7-bit)
//               bits 16..22 low band frequency  (code + 0x40 = 32*log2(f/10))
//               bits 23..29 low band amplitude  (7-bit)
//           (b) low 20 bits zero: one 5-bit amplitude command per band
//           (c) otherwise: one 7-bit value for a single band, then two
//               5-bit commands per band
//   mode 2: two 5-bit commands per band (or a 7-bit + 5-bit pair, then 5-bit)
//   mode 3: three 5-bit commands per band
//
// 5-bit commands are relative to the running amplitude (see apply_cmd5), which
// is why the decoder keeps per-motor state. The packed layouts follow the
// community reverse engineering of the format (also implemented by OpenPuck).
// ---------------------------------------------------------------------------

#define LVL_SILENT (-8.0f)
// log2(linear amplitude of code 127) on dekuNukem's curve: 2^(127/32) / 8.7
#define LVL_REF_LINEAR 1.8016f

float s1_amp_code_to_linear(int code) {
    if (code <= 0) return 0.0f;
    if (code > 127) code = 127;
    if (code < 16) return 0.1176f * (float)code / 16.0f;
    if (code < 32) return exp2f((float)code / 16.0f) / 17.0f;
    return exp2f((float)code / 32.0f) / 8.7f;
}

static float code7_to_lvl(int code) {
    float lin = s1_amp_code_to_linear(code);
    if (lin <= 0.0f) return LVL_SILENT;
    float l = log2f(lin / LVL_REF_LINEAR);
    return l < LVL_SILENT ? LVL_SILENT : l;
}

static float lvl_to_linear(float lvl) {
    if (lvl <= LVL_SILENT + 0.001f) return 0.0f;
    return exp2f(lvl) * LVL_REF_LINEAR;
}

static float hf_code_to_hz(int code) {
    return 10.0f * exp2f((float)(code + 0x60) / 32.0f);
}

static float lf_code_to_hz(int code) {
    return 10.0f * exp2f((float)(code + 0x40) / 32.0f);
}

static uint32_t field(uint32_t w, int shift, int width) {
    return (w >> shift) & ((1u << width) - 1u);
}

static float apply_cmd5(uint32_t c, float lvl) {
    if (c == 0) return LVL_SILENT;
    if (c <= 11) return -0.5f * (float)(c - 1);
    float step = 0.0f;
    if (c >= 17 && c <= 19) step = 0.125f;
    else if (c >= 20 && c <= 22) step = 0.03125f;
    else if (c >= 26 && c <= 28) step = -0.03125f;
    else if (c >= 29) step = -0.125f;
    else return lvl;  // frequency-only command: amplitude unchanged
    if (lvl <= LVL_SILENT) lvl = LVL_SILENT;
    lvl += step;
    if (lvl > 0.0f) lvl = 0.0f;
    if (lvl < LVL_SILENT) lvl = LVL_SILENT;
    return lvl;
}

void s1_rumble_reset(s1_rumble_state_t *st) {
    st->hi_freq_hz = 320.0f;
    st->lo_freq_hz = 160.0f;
    st->hi_lvl = LVL_SILENT;
    st->lo_lvl = LVL_SILENT;
}

static void emit(const s1_rumble_state_t *st, rumble_sample_t *out, int *n) {
    if (*n >= 3) return;
    out[*n].hi_freq_hz = st->hi_freq_hz;
    out[*n].lo_freq_hz = st->lo_freq_hz;
    out[*n].hi_amp = lvl_to_linear(st->hi_lvl);
    out[*n].lo_amp = lvl_to_linear(st->lo_lvl);
    (*n)++;
}

int s1_rumble_decode(s1_rumble_state_t *st, const uint8_t b[4], rumble_sample_t out[3]) {
    uint32_t w = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    int n = 0;

    if (w == 0) {
        // Some hosts send all zeroes to mean "stop"; never let that hold a buzz.
        st->hi_lvl = st->lo_lvl = LVL_SILENT;
        emit(st, out, &n);
        return n;
    }

    switch (field(w, 30, 2)) {
    case 0:
        emit(st, out, &n);
        break;
    case 1:
        if ((w & 0xFFFFFu) == 0) {
            st->lo_lvl = apply_cmd5(field(w, 25, 5), st->lo_lvl);
            st->hi_lvl = apply_cmd5(field(w, 20, 5), st->hi_lvl);
            emit(st, out, &n);
        } else if ((w & 0x3u) == 0) {
            st->hi_freq_hz = hf_code_to_hz((int)field(w, 2, 7));
            st->hi_lvl = code7_to_lvl((int)field(w, 9, 7));
            st->lo_freq_hz = lf_code_to_hz((int)field(w, 16, 7));
            st->lo_lvl = code7_to_lvl((int)field(w, 23, 7));
            emit(st, out, &n);
        } else {
            bool hi = (w & 1u) != 0;
            bool is_freq = (w & 4u) != 0;
            int v = (int)field(w, 23, 7);
            if (is_freq) {
                if (hi) st->hi_freq_hz = hf_code_to_hz(v);
                else st->lo_freq_hz = lf_code_to_hz(v);
            } else {
                if (hi) st->hi_lvl = code7_to_lvl(v);
                else st->lo_lvl = code7_to_lvl(v);
            }
            emit(st, out, &n);
            st->lo_lvl = apply_cmd5(field(w, 18, 5), st->lo_lvl);
            st->hi_lvl = apply_cmd5(field(w, 13, 5), st->hi_lvl);
            emit(st, out, &n);
            st->lo_lvl = apply_cmd5(field(w, 8, 5), st->lo_lvl);
            st->hi_lvl = apply_cmd5(field(w, 3, 5), st->hi_lvl);
            emit(st, out, &n);
        }
        break;
    case 2:
        if ((w & 0x3FFu) == 0) {
            st->lo_lvl = apply_cmd5(field(w, 25, 5), st->lo_lvl);
            st->hi_lvl = apply_cmd5(field(w, 20, 5), st->hi_lvl);
            emit(st, out, &n);
            st->lo_lvl = apply_cmd5(field(w, 15, 5), st->lo_lvl);
            st->hi_lvl = apply_cmd5(field(w, 10, 5), st->hi_lvl);
            emit(st, out, &n);
        } else {
            if (w & 1u) {
                st->hi_lvl = code7_to_lvl((int)field(w, 23, 7));
                st->lo_lvl = apply_cmd5(field(w, 18, 5), st->lo_lvl);
            } else {
                st->lo_lvl = code7_to_lvl((int)field(w, 23, 7));
                st->hi_lvl = apply_cmd5(field(w, 18, 5), st->hi_lvl);
            }
            emit(st, out, &n);
            st->lo_lvl = apply_cmd5(field(w, 13, 5), st->lo_lvl);
            st->hi_lvl = apply_cmd5(field(w, 8, 5), st->hi_lvl);
            emit(st, out, &n);
        }
        break;
    case 3:
        st->lo_lvl = apply_cmd5(field(w, 25, 5), st->lo_lvl);
        st->hi_lvl = apply_cmd5(field(w, 20, 5), st->hi_lvl);
        emit(st, out, &n);
        st->lo_lvl = apply_cmd5(field(w, 15, 5), st->lo_lvl);
        st->hi_lvl = apply_cmd5(field(w, 10, 5), st->hi_lvl);
        emit(st, out, &n);
        st->lo_lvl = apply_cmd5(field(w, 5, 5), st->lo_lvl);
        st->hi_lvl = apply_cmd5(field(w, 0, 5), st->hi_lvl);
        emit(st, out, &n);
        break;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Switch 2 HD rumble
//
// Each 5-byte frame packs four 10-bit fields (SDL's EncodeHDRumble):
//   bits 0..9 high band frequency, 10..19 high band amplitude,
//   bits 20..29 low band frequency, 30..39 low band amplitude.
// A packet for one actuator is 0x50|seq followed by three frames that the
// controller plays back in order.
//
// The frequency code -> Hz relationship has not been published. The console's
// idle frame uses 0x187 / 0x112, which we anchor to the Switch 1 neutral
// frequencies 320 Hz / 160 Hz. Both anchors fit a single logarithmic scale of
// 117 codes per octave (0x187 - 0x112 = 117), which is what we use by default;
// the slope is configurable from the web page in case real hardware disagrees,
// and "fixed" mode sidesteps the question by only translating amplitude.
// ---------------------------------------------------------------------------

void s2_rumble_encode_frame(uint16_t hi_freq, uint16_t hi_amp, uint16_t lo_freq, uint16_t lo_amp,
                            uint8_t out[S2_RUMBLE_FRAME_LEN]) {
    uint64_t v = 0;
    v |= (uint64_t)(hi_freq & 0x3FF);
    v |= (uint64_t)(hi_amp & 0x3FF) << 10;
    v |= (uint64_t)(lo_freq & 0x3FF) << 20;
    v |= (uint64_t)(lo_amp & 0x3FF) << 30;
    for (int i = 0; i < S2_RUMBLE_FRAME_LEN; i++) {
        out[i] = (uint8_t)(v >> (8 * i));
    }
}

uint16_t s2_rumble_freq_code(float hz, bool is_hi, const s2_rumble_params_t *p) {
    if (!p->translate_freq || !(hz > 1.0f)) {
        return is_hi ? S2_RUMBLE_NEUTRAL_HI_FREQ : S2_RUMBLE_NEUTRAL_LO_FREQ;
    }
    float code = (float)S2_RUMBLE_NEUTRAL_LO_FREQ + (float)p->freq_slope * log2f(hz / 160.0f);
    if (code < 1.0f) code = 1.0f;
    if (code > 1023.0f) code = 1023.0f;
    return (uint16_t)lroundf(code);
}

// Linear amplitude 1.0 (the Switch 1 nominal maximum) maps to this 10-bit value
// at 100% strength. SDL caps PC rumble at roughly 0.44 of full scale for the
// health of the actuators; we use the same ceiling for "1.0".
#define S2_AMP_AT_UNITY 450.0f

uint16_t s2_rumble_amp_code(float linear, const s2_rumble_params_t *p) {
    if (!(linear > 0.0f)) return 0;
    float a = linear * S2_AMP_AT_UNITY * (float)p->strength_pct / 100.0f;
    if (a > 1023.0f) a = 1023.0f;
    return (uint16_t)lroundf(a);
}

void s2_rumble_encode_block(uint8_t seq, const rumble_sample_t *samples, int n,
                            const s2_rumble_params_t *p, uint8_t out[S2_RUMBLE_BLOCK_LEN]) {
    out[0] = (uint8_t)(0x50 | (seq & 0x0F));
    for (int i = 0; i < 3; i++) {
        uint8_t *f = out + 1 + i * S2_RUMBLE_FRAME_LEN;
        if (n <= 0) {
            s2_rumble_encode_frame(S2_RUMBLE_NEUTRAL_HI_FREQ, 0, S2_RUMBLE_NEUTRAL_LO_FREQ, 0, f);
            continue;
        }
        const rumble_sample_t *s = &samples[i < n ? i : n - 1];
        uint16_t ha = s2_rumble_amp_code(s->hi_amp, p);
        uint16_t la = s2_rumble_amp_code(s->lo_amp, p);
        s2_rumble_encode_frame(s2_rumble_freq_code(s->hi_freq_hz, true, p), ha,
                               s2_rumble_freq_code(s->lo_freq_hz, false, p), la, f);
    }
}

bool s2_rumble_block_active(const uint8_t block[S2_RUMBLE_BLOCK_LEN]) {
    for (int i = 0; i < 3; i++) {
        const uint8_t *f = block + 1 + i * S2_RUMBLE_FRAME_LEN;
        uint64_t v = 0;
        for (int b = 0; b < S2_RUMBLE_FRAME_LEN; b++) v |= (uint64_t)f[b] << (8 * b);
        if (((v >> 10) & 0x3FF) || ((v >> 30) & 0x3FF)) return true;
    }
    return false;
}
