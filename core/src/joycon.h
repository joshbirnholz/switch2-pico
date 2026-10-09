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
//  * buttons: each side's own, with SL / SR as GL / GR (SL of either side
//    is GL, SR is GR);
//  * sticks: the (L)'s left, the (R)'s right; a single Joy-Con's stick turned
//    a quarter (as it is held sideways) and made the left stick;
//  * motion from the (R) when there is one (else the (L)), battery from the
//    lower one.
// `ctx` gets the matching stick calibrations and gyro scale (its other fields
// are left as they are).
void joycon_merge(ctrl_type_t type, const joycon_side_t *l, const joycon_side_t *r, s2_input_t *out,
                  mapping_ctx_t *ctx);

// A stick held a quarter turn left (an (L) sideways: `left` true) or right,
// as an upright stick: raw values and calibration.
void joycon_rotate_stick(bool left, const uint16_t raw[2], const s2_stick_cal_t *cal, uint16_t out[2],
                         s2_stick_cal_t *out_cal);

// Mouse movement from consecutive absolute sensor positions (they wrap).
typedef struct {
    uint16_t x, y;
    bool valid;
} joycon_mouse_track_t;

void joycon_mouse_reset(joycon_mouse_track_t *t);
// Movement since the previous position (0, 0 for the first one).
void joycon_mouse_delta(joycon_mouse_track_t *t, uint16_t x, uint16_t y, int32_t *dx, int32_t *dy);

// What the mouse buttons and wheel come from: the Joy-Con 2 `src` (mouse_src_t).
typedef struct {
    uint32_t left, right, middle;   // raw S2_BTN_* bit of each button
    bool stick_left;                // its stick is the left stick (else the right)
} joycon_mouse_buttons_t;
bool joycon_mouse_buttons(ctrl_type_t type, uint8_t src, joycon_mouse_buttons_t *out);

// Orient and scale a movement as the profile says (swap, inverts, speed).
// The result is in hundredths of a count (the caller keeps the remainder).
void joycon_mouse_apply(const profile_t *p, int32_t dx, int32_t dy, int32_t *ox, int32_t *oy);

#ifdef __cplusplus
}
#endif

#endif
