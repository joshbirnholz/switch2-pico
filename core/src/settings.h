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
    USB_MODE_DUALSENSE = 2,       // DualSense
    USB_MODE_XBOX360 = 3,         // wired Xbox 360 controller (XInput)
    USB_MODE_GC_ADAPTER = 4,      // Wii U / Switch GameCube controller adapter (WUP-028), port 1
    USB_MODE_COUNT
} usb_mode_t;

// Outputs of the non-Switch modes, by position (South = Cross / Xbox A).
// Each mode renders the ones it has (see usb_mode.c) and ignores the rest.
typedef enum {
    GP_NONE = 0,
    GP_SOUTH, GP_EAST, GP_WEST, GP_NORTH,
    GP_L1, GP_R1, GP_L2, GP_R2,
    GP_SELECT, GP_START, GP_L3, GP_R3, GP_GUIDE,
    GP_UP, GP_DOWN, GP_LEFT, GP_RIGHT,
    GP_TOUCHPAD,        // touchpad click, center
    GP_TP_LEFT,         // touchpad click, left half
    GP_TP_RIGHT,        // touchpad click, right half
    GP_MIC,
    GP_PADDLE_L, GP_PADDLE_R,
    GP_FN_L, GP_FN_R,
    GP_MACRO_QAM,       // tap: Guide, then Guide + South (Steam quick access)
    GP_COUNT
} gp_out_t;

#define MODE_MAP_SLOTS 8   // per-mode button maps (index = usb_mode_t); room for more modes

// Mode shortcut (hold C + Home, then press one of these): which USB mode each
// button selects, stored as usb_mode_t + 1 (0 = nothing on that button).
typedef enum {
    MODE_SLOT_A = 0, MODE_SLOT_B, MODE_SLOT_X, MODE_SLOT_Y,
    MODE_SLOT_UP, MODE_SLOT_DOWN, MODE_SLOT_LEFT, MODE_SLOT_RIGHT,
    MODE_SLOT_COUNT
} mode_slot_t;
#define MODE_SLOT_EMPTY 0
#define SETTINGS_EXT_REV 6   // 1: mode_map; 2: mode_slot; 3: ble_tx_power; 4: idle_*; 5: pair_button; 6: gc_profile

// Controller types with their own button maps and mode shortcut buttons.
typedef enum {
    CTRL_PRO = 0,        // Pro Controller 2 (and Joy-Con 2): button_map / mode_map / mode_slot
    CTRL_GAMECUBE = 1,   // NSO GameCube controller: gc_profile
    CTRL_TYPE_COUNT
} ctrl_type_t;

typedef struct {
    uint8_t button_map[IN_COUNT];                  // Switch Pro mode: out_button_t
    uint8_t mode_map[MODE_MAP_SLOTS][IN_COUNT];    // other modes: gp_out_t
    uint8_t mode_slot[MODE_SLOT_COUNT];            // mode shortcut buttons
} ctrl_profile_t;

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
    uint8_t ext_rev;                    // SETTINGS_EXT_REV; 0 in images saved before mode_map existed
    uint8_t mode_map[MODE_MAP_SLOTS][IN_COUNT];   // gp_out_t per button, for the non-Switch modes
    uint8_t mode_slot[MODE_SLOT_COUNT]; // usb_mode_t + 1 per mode_slot_t (MODE_SLOT_EMPTY: none)
    uint8_t ble_tx_power;               // radio power: 0 +8 dBm, 1 +4, 2 0, 3 -4 (nRF52840)
    uint8_t idle_disconnect_off;        // don't disconnect an unused controller (inverted: 0 = on)
    uint8_t idle_minutes;               // ... after this many minutes without input (1..240)
    uint8_t pair_button;                // pair only during a window opened by Sync / the page
                                        // (default on for boards with a button: Pico BOOTSEL)
    ctrl_profile_t gc_profile;          // NSO GameCube controller (the Pro uses the fields above)

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

// Default maps and mode shortcut buttons, per controller type.
void settings_default_button_map(ctrl_type_t type, uint8_t map[IN_COUNT]);              // Switch Pro mode
void settings_default_mode_map(ctrl_type_t type, usb_mode_t mode, uint8_t map[IN_COUNT]);  // other modes
void settings_default_mode_slots(uint8_t slots[MODE_SLOT_COUNT]);
void settings_default_profile(ctrl_type_t type, ctrl_profile_t *p);

// The maps of one controller type (the Pro's are the original fields).
static inline uint8_t *settings_button_map(const settings_t *s, ctrl_type_t t) {
    return (uint8_t *)(t == CTRL_GAMECUBE ? s->gc_profile.button_map : s->button_map);
}
static inline uint8_t *settings_mode_map(const settings_t *s, ctrl_type_t t, usb_mode_t m) {
    return (uint8_t *)(t == CTRL_GAMECUBE ? s->gc_profile.mode_map[m] : s->mode_map[m]);
}
static inline uint8_t *settings_mode_slots(const settings_t *s, ctrl_type_t t) {
    return (uint8_t *)(t == CTRL_GAMECUBE ? s->gc_profile.mode_slot : s->mode_slot);
}
// Map of the given USB mode (Switch Pro: out_button_t, others: gp_out_t).
static inline uint8_t *settings_map_for(const settings_t *s, ctrl_type_t t, usb_mode_t m) {
    return m == USB_MODE_SWITCH_PRO ? settings_button_map(s, t) : settings_mode_map(s, t, m);
}

const char *in_button_name(in_button_t b);
const char *out_button_name(out_button_t b);

#ifdef __cplusplus
}
#endif

#endif
