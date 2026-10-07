#ifndef S2P_PROCON_H
#define S2P_PROCON_H

#ifdef __cplusplus
extern "C" {
#endif

// Emulated Nintendo Switch (1) Pro Controller on USB: the 0x80 USB handshake,
// the 0x01 subcommand protocol (device info, SPI flash, report modes, IMU,
// vibration, player lights; the NFC/IR MCU only answers as idle), input report
// generation and HD rumble reception.

#include <stdbool.h>
#include <stdint.h>

#include "hd_rumble.h"
#include "mapping.h"

typedef struct {
    bool usb_mounted;
    bool handshake_done;        // host performed the 0x80 handshake
    uint8_t report_mode;        // 0 until the host selects one (0x30 / 0x31 / 0x3F)
    bool imu_enabled;
    bool vibration_enabled;
    uint8_t player_lights;
    uint32_t reports_sent;
    uint32_t rumble_frames;
    uint32_t subcommands;
} procon_status_t;

void procon_init(void);
void procon_task(void);
void procon_get_status(procon_status_t *out);

// Latest controller state to report. `connected` false sends neutral input.
void procon_set_input(const procon_input_t *in, bool connected, uint16_t battery_mv, bool charging);

// Body / button / grip colours shown by the host (RGB, 4 x 3 bytes).
void procon_set_colors(const uint8_t rgb[12]);

// USB plumbing (dispatched by usb_mode.c).
const uint8_t *procon_report_descriptor(uint16_t *len);
void procon_on_output(const uint8_t *buf, uint16_t len, bool via_control, uint8_t control_report_id);

// Hooks implemented by the firmware.
void procon_hook_rumble(const rumble_sample_t *left, int nl, const rumble_sample_t *right, int nr);
void procon_hook_player_lights(uint8_t lights);

#ifdef __cplusplus
}
#endif

#endif
