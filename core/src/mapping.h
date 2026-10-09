#ifndef S2P_MAPPING_H
#define S2P_MAPPING_H

#ifdef __cplusplus
extern "C" {
#endif

// Converts decoded Switch 2 input into the fields of a Switch 1 Pro Controller
// input report: button remapping, stick calibration + deadzones and the IMU
// axis / unit conversion. Pure C (unit tested on the host).

#include <stdbool.h>
#include <stdint.h>

#include "s2_proto.h"
#include "settings.h"

// Switch 1 Pro Controller button bits (3 bytes, little-endian: right, shared, left).
#define S1_BTN_Y        0x000001u
#define S1_BTN_X        0x000002u
#define S1_BTN_B        0x000004u
#define S1_BTN_A        0x000008u
#define S1_BTN_R        0x000040u
#define S1_BTN_ZR       0x000080u
#define S1_BTN_MINUS    0x000100u
#define S1_BTN_PLUS     0x000200u
#define S1_BTN_RSTICK   0x000400u
#define S1_BTN_LSTICK   0x000800u
#define S1_BTN_HOME     0x001000u
#define S1_BTN_CAPTURE  0x002000u
#define S1_BTN_DOWN     0x010000u
#define S1_BTN_UP       0x020000u
#define S1_BTN_RIGHT    0x040000u
#define S1_BTN_LEFT     0x080000u
#define S1_BTN_L        0x400000u
#define S1_BTN_ZL       0x800000u

// Stick values we emit are centered at 2048 and reach +-S1_STICK_RANGE at full
// deflection; the emulated SPI factory calibration advertises the same range.
#define S1_STICK_CENTER 2048
#define S1_STICK_RANGE  1800

// Switch 1 Pro Controller IMU scale (from its factory calibration):
//   accel 4096 LSB/g, gyro 13371/936 = 14.285 LSB per deg/s.
#define S1_ACCEL_LSB_PER_G   4096.0f
#define S1_GYRO_LSB_PER_DPS  (13371.0f / 936.0f)

// Switch 2 IMU: accel is +-8 g over int16 (4096 LSB/g). The gyro is +-2000 dps
// over int16 (16.4 LSB/dps) on most controllers; SDL observed a second mode
// (40 rad/s full scale, ~14.3 LSB/dps) on controllers whose IMU timestamps
// don't tick in microseconds. See s2_link.c for the detection.
#define S2_ACCEL_LSB_PER_G       4096.0f
#define S2_GYRO_LSB_PER_DPS_A    (32767.0f / (34.8f * 57.29578f))
#define S2_GYRO_LSB_PER_DPS_B    (32767.0f / (40.0f * 57.29578f))

typedef struct {
    s2_stick_cal_t cal_l;
    s2_stick_cal_t cal_r;
    bool is_gamecube;
    uint8_t gc_trigger_neutral[2];
    float gyro_lsb_per_dps;     // native scale of the connected controller
    ctrl_type_t type;           // whose profiles apply (CTRL_PRO when zeroed)
} mapping_ctx_t;

// Which controller's profiles apply.
static inline ctrl_type_t mapping_ctrl_type(const mapping_ctx_t *ctx) {
    if (!ctx) return CTRL_PRO;
    if (ctx->is_gamecube) return CTRL_GAMECUBE;
    return ctx->type < CTRL_TYPE_COUNT ? ctx->type : CTRL_PRO;
}

typedef struct {
    uint32_t buttons;           // S1_BTN_* (24 bits used)
    uint16_t stick_l[2];        // 12-bit, x/y
    uint16_t stick_r[2];
    int16_t accel[3];           // Switch 1 Pro Controller IMU frame and units
    int16_t gyro[3];
} procon_input_t;

uint32_t mapping_out_button_bit(out_button_t b);
uint32_t mapping_in_button_s2_bit(in_button_t b);

uint32_t mapping_buttons(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in);
// Same, ignoring the raw Switch 2 buttons in `skip` (S2_BTN_* bits).
uint32_t mapping_buttons_except(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in, uint32_t skip);
void mapping_stick(const settings_t *s, const s2_stick_cal_t *cal, const uint16_t raw[2], uint16_t out[2]);
void mapping_imu(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in,
                 int16_t accel_out[3], int16_t gyro_out[3]);
void mapping_apply(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in, procon_input_t *out);

// Generic (positional) buttons of the non-Switch modes: bit GP_BIT(gp_out_t).
#define GP_BIT(g) (1u << (g))
uint32_t mapping_gp_buttons(const settings_t *s, const uint8_t map[IN_COUNT], const mapping_ctx_t *ctx,
                            const s2_input_t *in);

// IMU in SDL's frame, which is also the DualSense's: accel in g (x right,
// y up, z towards the player), gyro in deg/s (x pitch, y yaw, z roll).
// Applies the gyro bias, scale settings and on/off switch.
void mapping_imu_sdl(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in, float accel_g[3],
                     float gyro_dps[3]);

// Quick remap from the controller: hold C and GL (or GR), then press another
// button to make GL (GR) send whatever that button sends; doing it again with
// the same button clears GL (GR). `prev`/`raw` are consecutive raw Switch 2
// button states. Returns true and sets *changed when a mapping changed.
bool mapping_quick_remap(settings_t *s, ctrl_type_t type, uint32_t prev, uint32_t raw, in_button_t *changed);
// Same on any button map (0 = unassigned), e.g. a non-Switch mode's map.
bool mapping_quick_remap_map(uint8_t map[IN_COUNT], uint32_t prev, uint32_t raw, in_button_t *changed);
// True while a quick-remap chord is held (its buttons shouldn't reach the host).
bool mapping_quick_remap_held(uint32_t raw);

// Macro outputs (OUT_HOME_A). A button mapped to a macro fires it when tapped
// on its own (released without another button being pressed meanwhile), so it
// can double as a chord modifier (C + GL/GR quick remap). Call for every input
// update and while mapping_macro_busy(); OR the result into the output.
typedef struct {
    uint32_t held;      // raw S2 bit of the macro button being held (0: none)
    bool spoiled;       // another button was pressed while it was held
    uint32_t start_ms;  // running macro start time
    bool running;
} mapping_macro_t;

#define MACRO_HOME_MS    60    // Home alone, then
#define MACRO_HOME_A_MS  100   // Home + A, then release

// Switch mode: returns the S1 buttons to add (Home, then Home + A).
uint32_t mapping_macro_step(mapping_macro_t *m, const settings_t *s, ctrl_type_t type, uint32_t prev, uint32_t raw,
                            uint32_t now_ms);

typedef enum { MACRO_IDLE, MACRO_GUIDE, MACRO_GUIDE_SOUTH } macro_phase_t;
// Any map: buttons mapped to `macro_value` run the macro; returns its phase.
macro_phase_t mapping_macro_run(mapping_macro_t *m, const uint8_t map[IN_COUNT], uint8_t macro_value, uint32_t prev,
                                uint32_t raw, uint32_t now_ms);
bool mapping_macro_busy(const mapping_macro_t *m);

void mapping_pack_stick(const uint16_t v[2], uint8_t out[3]);

#ifdef __cplusplus
}
#endif

#endif
