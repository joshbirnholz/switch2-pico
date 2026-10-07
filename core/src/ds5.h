#ifndef S2P_DS5_H
#define S2P_DS5_H

#ifdef __cplusplus
extern "C" {
#endif

// Emulated DualSense (054C:0CE6) and DualSense Edge (054C:0DF2) on USB, the
// alternatives to the Switch Pro Controller emulation in procon.c. Buttons come
// from the mode's generic map (gp_out_t). Steam Input, SDL and the Linux
// hid-playstation driver know the Edge's back paddles and Fn buttons, so the
// default Edge map gives GL / GR / C to them. Touchpad clicks can be sent as a
// left, centre or right touch.
//
// Implements the USB input report 0x01, output report 0x02 (rumble, player
// LEDs) and the feature reports hosts read at start-up (0x05 calibration,
// 0x09 pairing info, 0x20 firmware info). The report descriptor is our own:
// hosts parse these reports by ID and size, not via the descriptor.

#include <stdbool.h>
#include <stdint.h>

#include "mapping.h"
#include "procon.h"
#include "s2_proto.h"

void ds5_init(void);
void ds5_task(void);

// Latest controller state; `gp` holds the mapped GP_BIT() buttons. `in` NULL
// (or connected false) sends neutral input.
void ds5_set_input(const s2_input_t *in, const mapping_ctx_t *ctx, uint32_t gp, bool connected);

const uint8_t *ds5_report_descriptor(uint16_t *len);
// GET_REPORT(feature): fills buf (report ID first) and returns its length.
// Safe to call from the USB stack's context.
uint16_t ds5_get_feature(uint8_t report_id, uint8_t *buf, uint16_t len);
void ds5_on_output(const uint8_t *buf, uint16_t len, bool via_control, uint8_t control_report_id);
void ds5_get_status(procon_status_t *out);

// Pure helpers (unit tested).
// Input report 0x01 (64 bytes including the ID) from mapped state.
typedef struct {
    uint32_t gp;                // GP_BIT() buttons
    bool edge;                  // render paddles / Fn (DualSense Edge)
    uint8_t touch_id;           // DualSense touch tracking id (7 bits)
    uint16_t stick_l[2], stick_r[2];   // 12-bit, centre 2048, y up
    uint8_t trigger_l, trigger_r;      // 0..255
    float accel_g[3], gyro_dps[3];     // SDL / DualSense frame
    uint8_t battery_pct;               // 0..100
    bool charging;
} ds5_state_t;

#define DS5_INPUT_REPORT_LEN 64
void ds5_build_input(const ds5_state_t *st, uint8_t seq, uint32_t timestamp, uint8_t out[DS5_INPUT_REPORT_LEN]);

typedef struct {
    bool rumble;                // the report carries motor values
    uint8_t motor_left;         // strong / low frequency
    uint8_t motor_right;        // weak / high frequency
    bool player_leds;           // the report carries a player LED pattern
    uint8_t player_pattern;     // 5 bits, DualSense layout
} ds5_output_t;

// Parses output report 0x02 (`buf` starts with the report ID).
bool ds5_parse_output(const uint8_t *buf, uint16_t len, ds5_output_t *out);

#ifdef __cplusplus
}
#endif

#endif
