#ifndef S2P_SINPUT_H
#define S2P_SINPUT_H

#ifdef __cplusplus
extern "C" {
#endif

// Emulated SInput controller (Hand Held Legend's open HID protocol, 2E8A:10C6),
// an alternative to the Switch Pro Controller emulation in procon.c. SDL (and
// so Steam) reads its capabilities from the device: four back paddles,
// Capture and an extra button of its own come through as separate buttons,
// with Nintendo face labels, gyro and rumble. Buttons come from the mode's
// generic map (gp_out_t): GP_PADDLE_L / GP_FN_L are the left paddles 1 / 2,
// GP_PADDLE_R / GP_FN_R the right ones, GP_MIC is Capture, GP_MISC the extra
// button.
//
// Reports (layouts as SDL's SDL_hidapi_sinput.c reads them): input 0x01
// (64 bytes with the ID), command replies on input 0x02 and host commands
// on output 0x03 (48 bytes: haptics, features request, player LEDs).

#include <stdbool.h>
#include <stdint.h>

#include "mapping.h"
#include "procon.h"
#include "s2_proto.h"

void sinput_init(void);
void sinput_task(void);

// Latest controller state; `gp` holds the mapped GP_BIT() buttons. `in` NULL
// (or connected false) sends neutral input.
void sinput_set_input(const s2_input_t *in, const mapping_ctx_t *ctx, uint32_t gp, bool connected);

const uint8_t *sinput_report_descriptor(uint16_t *len);
void sinput_on_output(const uint8_t *buf, uint16_t len, bool via_control, uint8_t control_report_id);
void sinput_get_status(procon_status_t *out);

// Pure helpers (unit tested).
typedef struct {
    uint32_t gp;                       // GP_BIT() buttons
    uint16_t stick_l[2], stick_r[2];   // 12-bit, center 2048, y up
    uint8_t trigger_l, trigger_r;      // 0..255
    float accel_g[3], gyro_dps[3];     // SDL frame
    uint8_t battery_pct;               // 0..100
    bool charging;
} sinput_state_t;

#define SINPUT_REPORT_LEN 64
#define SINPUT_GYRO_RANGE_DPS 2000
#define SINPUT_ACCEL_RANGE_G  8

// Input report 0x01.
void sinput_build_input(const sinput_state_t *st, uint32_t timestamp_us, uint8_t out[SINPUT_REPORT_LEN]);
// The reply to the features command (input report 0x02).
void sinput_build_features(const uint8_t mac[6], uint8_t out[SINPUT_REPORT_LEN]);

typedef struct {
    bool features;          // the host asked for the features reply
    bool rumble;            // the command set the motors
    uint8_t motor_left, motor_right;
    bool player;            // the command set the player number
    uint8_t player_num;     // 1 = player 1; 0 = none
} sinput_command_t;

// Parses output report 0x03 (`buf` starts with the report ID).
bool sinput_parse_output(const uint8_t *buf, uint16_t len, sinput_command_t *out);

#ifdef __cplusplus
}
#endif

#endif
