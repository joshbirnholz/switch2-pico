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
    // Joy-Con 2 (R)'s SL / SR in a pair (a pair's GL / GR are the (L)'s SL /
    // SR; a single Joy-Con 2's SL / SR are GL / GR).
    IN_SL_R, IN_SR_R,
    IN_COUNT
} in_button_t;
// Inputs before IN_SL_R / IN_SR_R: the size of the maps stored before
// settings ext_rev 14 (the legacy fields keep it, so the layout stays put).
#define IN_COUNT_V1 21

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
    OUT_HOME_R,         // tap: Home, then Home + R (Steam screenshot)
    OUT_DECKY_QAM,      // opens the Quick Access menu through the Decky plugin (SteamOS)
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
    USB_MODE_SINPUT = 5,          // SInput (2E8A:10C6): paddles, Capture and an extra button for Steam Input
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
    GP_MACRO_SHOT,      // tap: Guide, then Guide + R1 (Steam screenshot)
    GP_DECKY_QAM,       // opens the Quick Access menu through the Decky plugin (SteamOS)
    GP_MISC,            // an extra button of its own (SInput)
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
#define SETTINGS_EXT_REV 15  // 1: mode_map; 2: mode_slot; 3: ble_tx_power; 4: idle_*; 5: pair_button;
                             // 6: gc_profile; 7: gc_profile sticks / rumble; 8: bonds;
                             // 9: gc_usb_mode, last_ctrl; 10: profiles; 11: one profile per button;
                             // 12: Joy-Con 2 types; 13: single Joy-Con 2 profiles numbered;
                             // 14: maps include IN_SL_R / IN_SR_R; 15: mouse_flags (MOUSE_FLAG_*)

// Remembered (paired) controllers. Any of them can connect, one at a time
// (or a Joy-Con 2 (L) together with a Joy-Con 2 (R)); pairing one more when
// the list is full forgets the oldest pairing.
#define BOND_MAX 8
typedef struct {
    uint8_t addr[6];                    // BD_ADDR, big-endian (as printed)
    uint8_t addr_type;
    uint8_t used;
    uint16_t pid;
    uint16_t reserved;
} bond_t;

// Controller types with their own profiles. A Joy-Con 2 on its own is held
// sideways; an (L) and an (R) connected together are one controller.
typedef enum {
    CTRL_PRO = 0,          // Nintendo Switch 2 Pro Controller
    CTRL_GAMECUBE = 1,     // Nintendo GameCube Controller
    CTRL_JOYCON_PAIR = 2,  // Joy-Con 2 (L/R)
    CTRL_JOYCON_L = 3,     // Joy-Con 2 (L), sideways
    CTRL_JOYCON_R = 4,     // Joy-Con 2 (R), sideways
    CTRL_TYPE_COUNT
} ctrl_type_t;

static inline bool ctrl_is_joycon(ctrl_type_t t) {
    return t == CTRL_JOYCON_PAIR || t == CTRL_JOYCON_L || t == CTRL_JOYCON_R;
}

// Mouse Mode (profile_t.mouse_src): a Joy-Con 2's optical sensor as a USB
// mouse. Either Joy-Con can be it, one at a time: the one lying on a surface
// (the first one put down, until it is lifted). Its shoulder clicks, its
// trigger right-clicks, its stick click middle-clicks and its stick scrolls.
typedef enum {
    MOUSE_OFF = 0,
    MOUSE_ON = 1,
} mouse_src_t;
// profile_t.mouse_flags. Off (the default): the stick's up / down scroll up /
// down and its left / right sideways; on: left / right scroll up / down too
// (as on a Switch 2).
#define MOUSE_FLAG_SCROLL_UP_DOWN_ONLY 0x01
// The stick's up / down, or its left / right, scroll the other way.
#define MOUSE_FLAG_INVERT_UP_DOWN      0x02
#define MOUSE_FLAG_INVERT_LEFT_RIGHT   0x04
#define MOUSE_FLAGS_ALL                0x07

// Profiles: a USB mode with a button map and the options that go with it.
// Each controller type has one per shortcut button (p[] is indexed by
// mode_slot_t: A, B, X, Y, D-pad up / down / left / right); one is in use,
// and C + Home, then a button, switches to that button's profile.
#define PROFILE_MAX      MODE_SLOT_COUNT
#define PROFILE_NAME_LEN 20
typedef struct {
    uint8_t used;
    uint8_t usb_mode;                   // usb_mode_t
    char name[PROFILE_NAME_LEN];        // NUL-terminated
    uint8_t map[IN_COUNT];              // Switch Pro mode: out_button_t; other modes: gp_out_t
    uint8_t stick_deadzone_pct;         // inner radial deadzone, 0..40
    uint8_t stick_outer_pct;            // radius treated as full deflection, 50..100
    uint8_t swap_sticks;                // swap left and right sticks
    uint8_t trigger_threshold;          // analog L / R count as pressed past this (GameCube controller)
    uint8_t rumble_enabled;
    uint8_t rumble_strength_pct;        // 0..200
    // Joy-Con 2 types only (zero elsewhere): the USB mouse.
    uint8_t mouse_src;                  // mouse_src_t
    uint8_t mouse_speed_pct;            // 10..250
    uint8_t mouse_flags;                // MOUSE_FLAG_* (before ext_rev 15: older options, cleared)
    uint8_t reserved[5];
} profile_t;

typedef struct {
    profile_t p[PROFILE_MAX];
    uint8_t slot[MODE_SLOT_COUNT];      // ext_rev 10 only (profile index + 1 per button); now unused
    uint8_t active;                     // profile in use
    uint8_t reserved[3];
} ctrl_profiles_t;

// Before profiles (ext_rev < 10): one map per mode. Read only to migrate.
typedef struct {
    uint8_t button_map[IN_COUNT_V1];                  // Switch Pro mode: out_button_t
    uint8_t mode_map[MODE_MAP_SLOTS][IN_COUNT_V1];    // other modes: gp_out_t
    uint8_t mode_slot[MODE_SLOT_COUNT];            // mode shortcut buttons
    // Sticks and rumble (the Pro's are the top-level fields of the same names).
    uint8_t stick_deadzone_pct;
    uint8_t stick_outer_pct;
    uint8_t swap_sticks;
    uint8_t rumble_enabled;
    uint8_t rumble_strength_pct;
} ctrl_profile_t;

#define SETTINGS_MAGIC   0x53325043u   // "S2PC"
#define SETTINGS_VERSION 1

#define SPI_USER_CAL_SIZE 0x40          // mirror of Pro Controller SPI 0x8000..0x803F

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;

    // ---- input mapping ----
    uint8_t button_map[IN_COUNT_V1];    // legacy: out_button_t for each physical button
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
    // The newest of `bonds` (kept in step by the settings_bond_* functions);
    // `bonded`: any controller paired.
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
    uint8_t mode_map[MODE_MAP_SLOTS][IN_COUNT_V1];   // legacy: gp_out_t per button, for the non-Switch modes
    uint8_t mode_slot[MODE_SLOT_COUNT]; // usb_mode_t + 1 per mode_slot_t (MODE_SLOT_EMPTY: none)
    uint8_t ble_tx_power;               // radio power: 0 +8 dBm, 1 +4, 2 0, 3 -4 (nRF52840)
    uint8_t idle_disconnect_off;        // don't disconnect an unused controller (inverted: 0 = on)
    uint8_t idle_minutes;               // ... after this many minutes without input (1..240)
    uint8_t pair_button;                // pair only during a window opened by Sync / the page
                                        // (default on for boards with a button: Pico BOOTSEL)
    ctrl_profile_t gc_profile;          // legacy: NSO GameCube controller (the Pro used the fields above)
    bond_t bonds[BOND_MAX];             // paired controllers, newest pairing first
    // USB mode per controller type: usb_mode is the Pro Controller's. The
    // dongle starts in the mode of the type that connected last and restarts
    // into the other type's mode when one of those connects.
    uint8_t gc_usb_mode;                // usb_mode_t for the NSO GameCube controller
    uint8_t last_ctrl;                  // ctrl_type_t of the controller that connected last
    // Profiles per controller type (ext_rev 10). They replace the per-mode
    // maps, mode_slot, usb_mode / gc_usb_mode and the stick / trigger /
    // rumble fields above, which only remain for migrating older saves.
    ctrl_profiles_t prof[CTRL_TYPE_COUNT];
    // A Joy-Con 2 connected on its own is its own controller (sideways, with
    // the Joy-Con 2 (L) / (R) profiles); off (default, and zero in older
    // saves): it is its half of the Joy-Con 2 (L/R).
    uint8_t joycon_single;
    uint8_t decky_options;              // the page shows the outputs that need the Decky plugin
    uint8_t reserved_end[2];

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

// Paired controllers (see bond_t).
int settings_bond_find(const settings_t *s, const uint8_t addr[6]);   // index or -1
int settings_bond_count(const settings_t *s);
// Remember a newly paired controller as the newest. Pairing one that is
// already known moves it to the front. Returns true and fills *dropped (if
// given) when the list was full and the oldest pairing was forgotten.
bool settings_bond_add(settings_t *s, const uint8_t addr[6], uint8_t addr_type, uint16_t pid, bond_t *dropped);
bool settings_bond_remove(settings_t *s, const uint8_t addr[6]);
void settings_bond_clear(settings_t *s);
// Point the single-controller fields (bonded, ctrl_*) at the newest pairing.
void settings_bond_sync(settings_t *s);

// Default maps and mode shortcut buttons, per controller type.
void settings_default_button_map(ctrl_type_t type, uint8_t map[IN_COUNT]);              // Switch Pro mode
void settings_default_mode_map(ctrl_type_t type, usb_mode_t mode, uint8_t map[IN_COUNT]);  // other modes
void settings_default_mode_slots(uint8_t slots[MODE_SLOT_COUNT]);
void settings_default_profile(ctrl_type_t type, ctrl_profile_t *p);

// Profiles (see profile_t).
void settings_default_profiles(ctrl_type_t type, ctrl_profiles_t *c);
void settings_default_profile_for(ctrl_type_t type, usb_mode_t mode, profile_t *p);   // default name / map / options
const char *settings_mode_profile_name(usb_mode_t mode);
void settings_sanitize_profiles(ctrl_type_t type, ctrl_profiles_t *c);
// ext_rev 10..13 -> 14: profiles as stored then (maps of IN_COUNT_V1), from
// `raw` (`n` controller types), into the current layout (the new inputs
// unassigned).
void settings_profiles_from_v13(const uint8_t *raw, int n, ctrl_profiles_t *out);
#define CTRL_PROFILES_V13_SIZE 468   // sizeof the stored ctrl_profiles_t then
// ext_rev 10 -> 11: put each profile on the button(s) that selected it,
// the others on free buttons.
void settings_profiles_to_buttons(ctrl_profiles_t *c);
// A single Joy-Con 2's profiles are numbered (no shortcut buttons); this
// packs them from the first (ext_rev 12 -> 13).
void settings_profiles_numbered(ctrl_profiles_t *c);
static inline bool profiles_on_buttons(ctrl_type_t t) {
    return t != CTRL_JOYCON_L && t != CTRL_JOYCON_R;
}

static inline ctrl_profiles_t *settings_profiles(const settings_t *s, ctrl_type_t t) {
    return (ctrl_profiles_t *)&s->prof[t < CTRL_TYPE_COUNT ? t : CTRL_PRO];
}
// The profile in use for a controller type (always a used one, see sanitize).
static inline profile_t *settings_active(const settings_t *s, ctrl_type_t t) {
    ctrl_profiles_t *c = settings_profiles(s, t);
    return &c->p[c->active < PROFILE_MAX ? c->active : 0];
}
static inline uint8_t *settings_active_map(const settings_t *s, ctrl_type_t t) {
    return settings_active(s, t)->map;
}
// The buttons with a profile, as mode_select.h takes them (index + 1, 0: none).
static inline void settings_profile_slots(const settings_t *s, ctrl_type_t t, uint8_t out[MODE_SLOT_COUNT]) {
    const ctrl_profiles_t *c = settings_profiles(s, t);
    for (int i = 0; i < MODE_SLOT_COUNT; i++) out[i] = c->p[i].used ? (uint8_t)(i + 1) : MODE_SLOT_EMPTY;
}
// The USB mode the dongle starts in: the active profile of the controller
// type that connected last.
static inline ctrl_type_t settings_boot_ctrl(const settings_t *s) {
    if (s->last_ctrl >= CTRL_TYPE_COUNT) return CTRL_PRO;
    // Single Joy-Con 2 not allowed: it was (and is) half of the pair.
    if (!s->joycon_single && (s->last_ctrl == CTRL_JOYCON_L || s->last_ctrl == CTRL_JOYCON_R)) return CTRL_JOYCON_PAIR;
    return (ctrl_type_t)s->last_ctrl;
}
static inline uint8_t settings_boot_usb_mode(const settings_t *s) {
    return settings_active(s, settings_boot_ctrl(s))->usb_mode;
}
// Whether a profile adds the USB mouse: Joy-Con 2 types, emulating an Xbox
// 360 or an SInput controller only (in the Switch Pro and DualSense modes,
// Linux's drivers for those controllers claim the mouse interface too and it
// gets no driver). Otherwise the mouse setting is kept but does nothing.
static inline bool settings_mode_has_mouse(uint8_t usb_mode) {
    return usb_mode == USB_MODE_XBOX360 || usb_mode == USB_MODE_SINPUT;
}
static inline bool settings_profile_mouse(const profile_t *p, ctrl_type_t t) {
    return ctrl_is_joycon(t) && p->mouse_src != MOUSE_OFF && settings_mode_has_mouse(p->usb_mode);
}
// ... for the profile the dongle starts with (the USB descriptors follow it).
static inline bool settings_boot_mouse(const settings_t *s) {
    return settings_profile_mouse(settings_active(s, settings_boot_ctrl(s)), settings_boot_ctrl(s));
}
// Sticks, triggers and rumble of a controller type's active profile.
typedef struct {
    uint8_t deadzone, outer, swap, trigger_threshold, rumble_enabled, rumble_strength;
} ctrl_tuning_t;
static inline ctrl_tuning_t settings_tuning(const settings_t *s, ctrl_type_t t) {
    const profile_t *p = settings_active(s, t);
    ctrl_tuning_t r = {p->stick_deadzone_pct, p->stick_outer_pct, p->swap_sticks,
                       p->trigger_threshold,  p->rumble_enabled,  p->rumble_strength_pct};
    return r;
}

const char *in_button_name(in_button_t b);
const char *ctrl_type_name(ctrl_type_t t);
const char *out_button_name(out_button_t b);

#ifdef __cplusplus
}
#endif

#endif
