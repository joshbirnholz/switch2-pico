#ifndef S2P_S2_PROTO_H
#define S2P_S2_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

// Nintendo Switch 2 controller Bluetooth LE protocol: identifiers, GATT UUIDs,
// command framing and report parsing. No hardware dependencies so it can be
// unit tested on the host (see test/).
//
// Protocol knowledge comes from the community research in
//   * ndeadly/switch2_controller_research   (GATT table, commands, reports)
//   * trevlars/switch2-controllers-linux     (working Linux implementation)
//   * libsdl-org/SDL SDL_hidapi_switch2.c     (calibration, IMU, rumble encoding)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define S2_NINTENDO_COMPANY_ID 0x0553
#define S2_NINTENDO_VID        0x057E

#define S2_PID_JOYCON2_R   0x2066
#define S2_PID_JOYCON2_L   0x2067
#define S2_PID_PRO2        0x2069
#define S2_PID_GAMECUBE    0x2073

// GATT UUIDs, in the usual printed (big-endian) byte order, as used by BTstack.
extern const uint8_t S2_UUID_HID_SERVICE[16];      // ab7de9be-...-fd0
extern const uint8_t S2_UUID_INPUT_COMMON[16];     // ab7de9be-...-fd2 (input report 0x05)
extern const uint8_t S2_UUID_CMD_WRITE[16];        // 649d4ac9-... command output
extern const uint8_t S2_UUID_CMD_RESPONSE[16];     // c765a961-... command response
extern const uint8_t S2_UUID_VIB_PRO[16];          // cc483f51-...-b05 Pro Controller 2 rumble
extern const uint8_t S2_UUID_VIB_GC[16];           // 3f8fb670-... GameCube rumble
// Controller-specific input reports (0x09 Pro Controller 2, 0x0A GameCube);
// byte 1 is Power Info: bit 0 external power, bit 1 charging, bits 2-5 the
// controller's own battery level 0-9.
extern const uint8_t S2_UUID_INPUT_PRO[16];        // 7492866c-...-0f8
extern const uint8_t S2_UUID_INPUT_GC[16];         // 8261cba1-...-d8e4d
extern const uint8_t S2_UUID_INPUT_JOYCON_L[16];   // cc1bbbb5-... (report 0x07)
extern const uint8_t S2_UUID_INPUT_JOYCON_R[16];   // d5a9e01e-... (report 0x08)
extern const uint8_t S2_UUID_VIB_JOYCON_L[16];
extern const uint8_t S2_UUID_VIB_JOYCON_R[16];
extern const uint8_t S2_UUID_REPORT_RATE_DESC[16]; // 679d5510-... "set report rate?" descriptor
extern const uint8_t S2_UUID_UNKNOWN_SVC_WRITE[16];// 00c5af5d-...-bd282 (console writes 01 00 here first)

// ---- commands ----
#define S2_CMD_MEMORY     0x02
#define S2_CMD_INIT       0x03
#define S2_CMD_UNK07      0x07
#define S2_CMD_LEDS       0x09
#define S2_CMD_VIBRATION  0x0A
#define S2_CMD_BATTERY    0x0B
#define S2_CMD_FEATURE    0x0C
#define S2_CMD_FW_INFO    0x10
#define S2_CMD_UNK11      0x11
#define S2_CMD_PAIR       0x15
#define S2_CMD_UNK16      0x16

#define S2_SUB_MEMORY_READ        0x04
#define S2_SUB_LEDS_SET_PATTERN   0x07
#define S2_SUB_VIB_PLAY_SAMPLE    0x02
#define S2_SUB_VIB_SEND_DATA      0x08
#define S2_SUB_FEATURE_SET_MASK   0x02
#define S2_SUB_FEATURE_ENABLE     0x04
#define S2_SUB_PAIR_SET_ADDRESS   0x01
#define S2_SUB_PAIR_CONFIRM_LTK   0x02
#define S2_SUB_PAIR_FINALIZE      0x03
#define S2_SUB_PAIR_EXCHANGE_KEY  0x04

#define S2_FEATURE_BUTTONS  0x01
#define S2_FEATURE_STICKS   0x02
#define S2_FEATURE_IMU      0x04
#define S2_FEATURE_UNK08    0x08
#define S2_FEATURE_MOUSE    0x10
#define S2_FEATURE_RUMBLE   0x20
#define S2_FEATURE_MAG      0x80

// Memory addresses.
#define S2_ADDR_DEVICE_INFO      0x00013000
#define S2_ADDR_FACTORY_STICK_L  0x00013080  // stick calibration at +0x28
#define S2_ADDR_FACTORY_STICK_R  0x000130C0
#define S2_ADDR_GC_TRIGGERS      0x00013140
#define S2_ADDR_USER_STICK_L     0x001FC040  // magic b2 a1, calibration at +2
#define S2_ADDR_USER_STICK_R     0x001FC080

#define S2_CMD_HEADER_LEN 8
#define S2_MEMORY_READ_MAX 0x40

// Fixed public key the controller always answers with during pairing.
extern const uint8_t S2_PAIR_DEVICE_KEY_B1[16];

// ---- advertisement ----
typedef struct {
    uint16_t vid;
    uint16_t pid;
    bool wake_console;          // controller asks a console to wake up
    uint8_t host_addr_le[6];    // host it wants to reconnect to; all zero in pairing mode
    bool pairing_mode;
} s2_adv_info_t;

// Parse the manufacturer specific AD structure payload (starting with the
// little-endian company id). Returns false if this is not a Switch 2 controller.
bool s2_parse_manufacturer_data(const uint8_t *data, size_t len, s2_adv_info_t *out);

// ---- commands ----
size_t s2_build_command(uint8_t *out, size_t out_cap, uint8_t cmd, uint8_t sub,
                        const uint8_t *data, size_t data_len);
size_t s2_build_memory_read(uint8_t *out, size_t out_cap, uint32_t address, uint8_t length);

typedef struct {
    uint8_t cmd;
    uint8_t sub;
    uint8_t ack;
    const uint8_t *data;
    size_t data_len;
} s2_response_t;

bool s2_parse_response(const uint8_t *buf, size_t len, s2_response_t *out);

// Memory read replies echo the length and address before the payload.
bool s2_parse_memory_read(const s2_response_t *rsp, uint32_t address, uint8_t length,
                          const uint8_t **payload);

// ---- input report 0x05 (characteristic ab7de9be-...-fd2) ----
#define S2_BTN_Y        (1u << 0)
#define S2_BTN_X        (1u << 1)
#define S2_BTN_B        (1u << 2)
#define S2_BTN_A        (1u << 3)
#define S2_BTN_SR_R     (1u << 4)
#define S2_BTN_SL_R     (1u << 5)
#define S2_BTN_R        (1u << 6)
#define S2_BTN_ZR       (1u << 7)
#define S2_BTN_MINUS    (1u << 8)
#define S2_BTN_PLUS     (1u << 9)
#define S2_BTN_RSTICK   (1u << 10)
#define S2_BTN_LSTICK   (1u << 11)
#define S2_BTN_HOME     (1u << 12)
#define S2_BTN_CAPTURE  (1u << 13)
#define S2_BTN_C        (1u << 14)
#define S2_BTN_DOWN     (1u << 16)
#define S2_BTN_UP       (1u << 17)
#define S2_BTN_RIGHT    (1u << 18)
#define S2_BTN_LEFT     (1u << 19)
#define S2_BTN_SR_L     (1u << 20)
#define S2_BTN_SL_L     (1u << 21)
#define S2_BTN_L        (1u << 22)
#define S2_BTN_ZL       (1u << 23)
#define S2_BTN_GR       (1u << 24)
#define S2_BTN_GL       (1u << 25)
#define S2_BTN_HEADSET  (1u << 28)

#define S2_INPUT_REPORT_MIN_LEN 0x3C

typedef struct {
    uint32_t counter;
    uint32_t buttons;
    uint16_t stick_l[2];        // raw 12-bit x, y
    uint16_t stick_r[2];
    uint16_t battery_mv;
    uint8_t charge_state;
    uint32_t imu_timestamp;     // microseconds on most controllers
    int16_t temperature;
    int16_t accel[3];           // raw, +-8 g full scale
    int16_t gyro[3];            // raw
    uint8_t trigger_l;          // GameCube analog triggers (raw)
    uint8_t trigger_r;
    // Joy-Con 2 optical sensor (feature S2_FEATURE_MOUSE): an absolute
    // position that wraps; deltas between reports are the movement.
    uint16_t mouse_x, mouse_y;
    // Seen on hardware: about 2600-4600 / 140 on a surface, 4600 / 3000 in
    // the air, so the second looks like the lift-off distance.
    uint16_t mouse_quality, mouse_distance;
} s2_input_t;

// Buttons that are on each Joy-Con 2 (report 0x05 uses one layout for all).
#define S2_BTNS_JOYCON_L (S2_BTN_DOWN | S2_BTN_UP | S2_BTN_RIGHT | S2_BTN_LEFT | S2_BTN_SR_L | S2_BTN_SL_L | \
                          S2_BTN_L | S2_BTN_ZL | S2_BTN_MINUS | S2_BTN_LSTICK | S2_BTN_CAPTURE)
#define S2_BTNS_JOYCON_R (S2_BTN_Y | S2_BTN_X | S2_BTN_B | S2_BTN_A | S2_BTN_SR_R | S2_BTN_SL_R | S2_BTN_R | \
                          S2_BTN_ZR | S2_BTN_PLUS | S2_BTN_RSTICK | S2_BTN_HOME | S2_BTN_C)

bool s2_parse_input_report(const uint8_t *data, size_t len, s2_input_t *out);

// ---- stick calibration ----
typedef struct {
    uint16_t center[2];
    uint16_t max[2];   // deflection above center
    uint16_t min[2];   // deflection below center
    bool valid;
} s2_stick_cal_t;

void s2_parse_stick_cal(const uint8_t data[9], s2_stick_cal_t *out);
void s2_default_stick_cal(s2_stick_cal_t *out);
// Normalise a raw axis to -1..+1 using the calibration.
float s2_stick_axis(const s2_stick_cal_t *cal, int axis, uint16_t raw);

// Player LED bit patterns (as used by the Switch) for players 1..8.
uint8_t s2_led_pattern_for_player(int player);

// Unpack two packed 12-bit values.
void s2_unpack12(const uint8_t b[3], uint16_t *a, uint16_t *c);

#ifdef __cplusplus
}
#endif

#endif
