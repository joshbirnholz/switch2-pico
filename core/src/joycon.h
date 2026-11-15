#ifndef S2P_JOYCON_H
#define S2P_JOYCON_H

// Joy-Con 2: one controller from a Joy-Con 2 (L) and (R), or a single one
// held sideways, and its optical sensor as a mouse. Pure logic (no
// hardware), covered by test/test_main.c.

#include <stdbool.h>
#include <stdint.h>

#include "mapping.h"
#include "s2_proto.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

// One Joy-Con 2 as its link sees it.
typedef struct {
    bool present;               // connected and sending input
    s2_input_t in;
    s2_stick_cal_t cal;         // its stick (the left stick of an (L), the right of an (R))
    float gyro_lsb_per_dps;
} joycon_side_t;

// The controller of type `type` (CTRL_JOYCON_*) from its sides (either may
// be missing):
//  * buttons: each side's own; on a pair the (L)'s SL / SR are GL / GR and
//    the (R)'s their own inputs (IN_SL_R / IN_SR_R), on a single Joy-Con 2
//    its SL / SR are GL / GR;
//  * sticks: the (L)'s left, the (R)'s right; a single Joy-Con's stick turned
//    a quarter (as it is held sideways) and made the left stick;
//  * motion from the preferred side (or the available side if it is absent);
//    battery from the lower one, independently of the motion source.
// `ctx` gets the matching stick calibrations and gyro scale (its other fields
// are left as they are).
void joycon_merge(ctrl_type_t type, joycon_gyro_side_t gyro_side, const joycon_side_t *l, const joycon_side_t *r, s2_input_t *out,
                  mapping_ctx_t *ctx);

// A stick held a quarter turn left (an (L) sideways: `left` true) or right,
// as an upright stick: raw values and calibration.
void joycon_rotate_stick(bool left, const uint16_t raw[2], const s2_stick_cal_t *cal, uint16_t out[2],
                         s2_stick_cal_t *out_cal);

// Whether a Joy-Con 2 is held about as a mouse lies (on the side with the
// rail): gravity along its width (accelerometer X), within about 55 degrees
// to come in, 65 to go out (`was`: the previous answer). Either way up
// counts. Readings far from 1 g (moving) keep the previous answer. As on a
// Switch 2, the optical sensor runs only then (it costs battery).
bool joycon_mouse_pose(const int16_t accel[3], bool was);

// Mouse movement from consecutive absolute sensor positions (they wrap).
typedef struct {
    uint16_t x, y;
    bool valid;
} joycon_mouse_track_t;

void joycon_mouse_reset(joycon_mouse_track_t *t);
// Movement since the previous position (0, 0 for the first one).
void joycon_mouse_delta(joycon_mouse_track_t *t, uint16_t x, uint16_t y, int32_t *dx, int32_t *dy);

// What the mouse buttons and wheel come from: the Joy-Con 2 (L) (`left`) or
// (R) that is the mouse, as controller type `type` sees it.
typedef struct {
    uint32_t left, right, middle;   // raw S2_BTN_* bit of each button
    bool stick_left;                // its stick is the left stick (else the right)
} joycon_mouse_buttons_t;
void joycon_mouse_buttons(ctrl_type_t type, bool left, joycon_mouse_buttons_t *out);

// Scrolling from the mouse Joy-Con's stick (`left`: an (L)), its axes
// -1..1 as s2_stick_axis() gives them. The stronger direction counts: up /
// down scroll up / down (`wheel`, up positive); left / right scroll sideways
// (`pan`), or with MOUSE_FLAG_SCROLL_UP_DOWN_ONLY up (left) / down (right);
// MOUSE_FLAG_INVERT_* turn either the other way.
void joycon_mouse_scroll(bool left, float x, float y, uint8_t flags, float *wheel, float *pan);

// Scale a movement by the profile's speed. The result is in hundredths of a
// count (the caller keeps the remainder).
void joycon_mouse_apply(const profile_t *p, int32_t dx, int32_t dy, int32_t *ox, int32_t *oy);

// ---- Gyro mouse (see GYRO_MOUSE_FLAG_* in settings.h) ----
// Pointer counts per degree turned at 100% speed.
#define GYRO_MOUSE_COUNTS_PER_DEG 20.0f

// Which Joy-Con 2 moves the pointer, as the USB mouse index (0: the (L), 1:
// the (R)), from what is connected (`has_l`, `has_r`); -1: none. The same
// one whose motion the controller reports (joycon_merge()).
int joycon_gyro_mouse_side(ctrl_type_t type, joycon_gyro_side_t gyro_side, bool has_l, bool has_r);

// A button combination, given as in_button_t bits, as raw S2_BTN_* bits.
uint32_t joycon_combo_raw(uint32_t in_bits);
// The combination became fully held between `prev` and `raw` (raw S2_BTN_*
// bits; an empty combination never is).
static inline bool joycon_combo_pressed(uint32_t combo_raw, uint32_t prev, uint32_t raw) {
    return combo_raw && (raw & combo_raw) == combo_raw && (prev & combo_raw) != combo_raw;
}
static inline bool joycon_combo_held(uint32_t combo_raw, uint32_t raw) {
    return combo_raw && (raw & combo_raw) == combo_raw;
}

typedef struct {
    bool have_up;
    float up[3];        // smoothed gravity direction (up), in the Joy-Con's own axes
} joycon_gyro_mouse_t;

void joycon_gyro_mouse_reset(joycon_gyro_mouse_t *g);
// Pointer movement (counts, right / down positive) for one motion sample of
// the Joy-Con at USB mouse index `side` (see joycon_gyro_mouse_side()):
// `gyro_dps` its turn rate and `accel` its raw accelerometer (+-8 g), both in
// its own axes, over `dt` seconds. Turning around the vertical (from the
// accelerometer) moves the pointer left / right whichever way the Joy-Con is
// held; tilting it about its width (a single Joy-Con 2 held sideways: its
// length) up / down. Small, slow turns (hand tremor) are damped.
void joycon_gyro_mouse_step(joycon_gyro_mouse_t *g, ctrl_type_t type, int side, const profile_t *p,
                            const float gyro_dps[3], const int16_t accel[3], float dt, float *dx, float *dy);

#ifdef __cplusplus
}
#endif

#endif
