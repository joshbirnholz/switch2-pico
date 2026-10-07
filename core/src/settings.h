#ifndef S2P_SETTINGS_H
#define S2P_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

// Physical buttons on the Switch 2 controller that can be remapped.
typedef enum {
    IN_A = 0, IN_B, IN_X, IN_Y,
    IN_L, IN_R, IN_ZL, IN_ZR,
    IN_MINUS, IN_PLUS, IN_LSTICK, IN_RSTICK,
    IN_HOME, IN_CAPTURE,
    IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT,
    IN_GL, IN_GR, IN_C,
    IN_COUNT
} in_button_t;

// Buttons of the emulated Switch (1) Pro Controller.
typedef enum {
    OUT_NONE = 0,
    OUT_A, OUT_B, OUT_X, OUT_Y,
    OUT_L, OUT_R, OUT_ZL, OUT_ZR,
    OUT_MINUS, OUT_PLUS, OUT_LSTICK, OUT_RSTICK,
    OUT_HOME, OUT_CAPTURE,
    OUT_UP, OUT_DOWN, OUT_LEFT, OUT_RIGHT,
    // Macros (no single Pro Controller button; see mapping_macro_step()).
    OUT_HOME_A,         // tap: Home, then Home + A (Steam quick access menu)
    OUT_COUNT
} out_button_t;

typedef enum {
    GYRO_RANGE_AUTO = 0,   // detect from the IMU timestamp behaviour (as SDL does)
    GYRO_RANGE_16_4 = 1,   // 16.4 LSB per deg/s (+-2000 dps over int16)
    GYRO_RANGE_14_3 = 2,   // 14.3 LSB per deg/s (same as a Switch 1 Pro Controller)
} gyro_range_t;

typedef enum {
    RUMBLE_FREQ_TRANSLATE = 0, // convert the host's HD rumble frequencies
    RUMBLE_FREQ_FIXED = 1,     // amplitude only, at the Switch 2 neutral frequencies
} rumble_freq_mode_t;

// What the dongle presents itself as on USB.
typedef enum {
    USB_MODE_SWITCH_PRO = 0,      // wired Switch (1) Pro Controller: Switch consoles, Steam, SDL
    USB_MODE_DUALSENSE_EDGE = 1,  // DualSense Edge: GL/GR/C become paddles/Fn that Steam Input can map
    USB_MODE_COUNT
} usb_mode_t;

#define SETTINGS_MAGIC   0x53325043u   // "S2PC"
#define SETTINGS_VERSION 1

#define SPI_USER_CAL_SIZE 0x40          // mirror of Pro Controller SPI 0x8000..0x803F

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;

    // ---- input mapping ----
    uint8_t button_map[IN_COUNT];       // out_button_t for each physical button
    uint8_t stick_deadzone_pct;         // inner radial deadzone, 0..40
    uint8_t stick_outer_pct;            // radius treated as full deflection, 50..100
    uint8_t swap_sticks;                // swap left and right sticks
    uint8_t gc_trigger_threshold;       // GameCube analog trigger -> ZL/ZR threshold (0..255)

    // ---- motion ----
    uint8_t gyro_enabled;
    uint8_t gyro_range;                 // gyro_range_t
    uint16_t gyro_scale_pct;            // 10..400, applied on top of the unit conversion
    uint16_t accel_scale_pct;           // 10..400
    int16_t gyro_bias[3];               // raw Switch 2 units, subtracted before conversion

    // ---- rumble ----
    uint8_t rumble_enabled;
    uint8_t rumble_strength_pct;        // 0..200
    uint8_t rumble_freq_mode;           // rumble_freq_mode_t
    uint8_t rumble_freq_slope;          // Switch 2 frequency codes per octave (see hd_rumble.c)

    uint8_t reserved_nfc[2];            // formerly NFC settings; keeps the stored layout

    // ---- USB ----
    uint8_t usb_report_interval_ms;     // 0x30 input report period (4..16)
    uint8_t led_follow_host;            // mirror the host's player LEDs on the controller
    uint8_t usb_detach_when_idle;       // only attach to USB while a controller is connected
    uint8_t usb_remote_wakeup;          // wake a sleeping host when the controller wakes up

    // ---- Wi-Fi configuration access point ----
    uint8_t hotkey_enabled;             // hold C + Home for 3 s to toggle the access point
    uint8_t wifi_autostart;             // start the access point at boot when no controller is paired
    uint8_t wifi_channel;
    char wifi_ssid[33];
    char wifi_pass[65];

    // ---- pairing (written by the firmware) ----
    uint8_t bonded;
    uint8_t ctrl_addr[6];               // controller BD_ADDR, big-endian (as printed)
    uint8_t ctrl_addr_type;
    uint16_t ctrl_pid;
    uint8_t ltk[16];                    // key from the Nintendo pairing exchange (informational)

    // ---- emulated Pro Controller user calibration (written by the host) ----
    uint8_t spi_user_cal[SPI_USER_CAL_SIZE];

    // ---- added later: new fields go here, just before `crc`, so settings
    // saved by older firmware still load (see settings_init) ----
    uint8_t webusb_enabled;             // expose the WebUSB configuration interface
    // Inverted so images saved before it existed (zero padding here) keep the
    // shortcut on.
    uint8_t quick_remap_off;            // disable the C + GL/GR + button remap shortcut
    uint8_t usb_mode;                   // usb_mode_t (0 in images saved before it existed)

    uint32_t crc;
} settings_t;

extern settings_t g_settings;

void settings_init(void);               // load from flash or fall back to defaults
void settings_defaults(settings_t *s);
void settings_sanitize(settings_t *s);
// Request a save; the actual flash write is debounced and performed from
// settings_task() so bursts of changes cost a single erase.
void settings_save_later(void);
void settings_save_now(void);
void settings_task(void);
void settings_factory_reset(void);

const char *in_button_name(in_button_t b);
const char *out_button_name(out_button_t b);

#ifdef __cplusplus
}
#endif

#endif
