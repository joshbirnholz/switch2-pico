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

// Stick values we emit are centred at 2048 and reach +-S1_STICK_RANGE at full
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
} mapping_ctx_t;

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
void mapping_stick(const settings_t *s, const s2_stick_cal_t *cal, const uint16_t raw[2], uint16_t out[2]);
void mapping_imu(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in,
                 int16_t accel_out[3], int16_t gyro_out[3]);
void mapping_apply(const settings_t *s, const mapping_ctx_t *ctx, const s2_input_t *in, procon_input_t *out);

void mapping_pack_stick(const uint16_t v[2], uint8_t out[3]);

#ifdef __cplusplus
}
#endif

#endif
