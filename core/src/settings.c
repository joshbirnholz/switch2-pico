#include "settings.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "platform.h"

settings_t g_settings;

_Static_assert(sizeof(settings_t) <= 4096, "settings_t too large");

static bool s_dirty;
static uint32_t s_save_deadline;

static const char *const IN_NAMES[IN_COUNT] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Minus", "Plus", "LStick", "RStick",
    "Home", "Capture", "Up", "Down", "Left", "Right", "GL", "GR", "C",
};

static const char *const OUT_NAMES[OUT_COUNT] = {
    "None", "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Minus", "Plus", "LStick", "RStick",
    "Home", "Capture", "Up", "Down", "Left", "Right",
    "Home+A (Steam quick access)",
};

static const char *const CTRL_NAMES[CTRL_TYPE_COUNT] = {
    [CTRL_PRO] = "Nintendo Switch 2 Pro Controller",
    [CTRL_GAMECUBE] = "Nintendo GameCube Controller",
    [CTRL_JOYCON_PAIR] = "Joy-Con 2 (L/R)",
    [CTRL_JOYCON_L] = "Joy-Con 2 (L)",
    [CTRL_JOYCON_R] = "Joy-Con 2 (R)",
};

const char *ctrl_type_name(ctrl_type_t t) {
    return t < CTRL_TYPE_COUNT ? CTRL_NAMES[t] : "?";
}

const char *in_button_name(in_button_t b) {
    return b < IN_COUNT ? IN_NAMES[b] : "?";
}

const char *out_button_name(out_button_t b) {
    return b < OUT_COUNT ? OUT_NAMES[b] : "?";
}

static uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t settings_crc(const settings_t *s) {
    return crc32((const uint8_t *)s, offsetof(settings_t, crc));
}

// CRC of settings written by firmware whose settings_t was `size` bytes long
// (the CRC is always the last field).
static bool stored_crc_ok(const uint8_t *raw, uint16_t size) {
    uint32_t crc;
    memcpy(&crc, raw + size - 4, 4);
    return crc == crc32(raw, size - 4u);
}

static void default_ssid(char *out, size_t len) {
    uint8_t id[PLATFORM_UNIQUE_ID_LEN];
    platform_unique_id(id);
    snprintf(out, len, "Switch2-Pico-%02X%02X", id[PLATFORM_UNIQUE_ID_LEN - 2], id[PLATFORM_UNIQUE_ID_LEN - 1]);
}

void settings_defaults(settings_t *s) {
    memset(s, 0, sizeof *s);
    s->magic = SETTINGS_MAGIC;
    s->version = SETTINGS_VERSION;
    s->size = sizeof *s;

    // Pro Controller maps (the original fields) and the GameCube controller's.
    settings_default_button_map(CTRL_PRO, s->button_map);
    for (int m = 0; m < MODE_MAP_SLOTS; m++) settings_default_mode_map(CTRL_PRO, (usb_mode_t)m, s->mode_map[m]);
    settings_default_mode_slots(s->mode_slot);
    settings_default_profile(CTRL_GAMECUBE, &s->gc_profile);
    s->gc_usb_mode = USB_MODE_GC_ADAPTER;
    for (int t = 0; t < CTRL_TYPE_COUNT; t++) settings_default_profiles((ctrl_type_t)t, &s->prof[t]);
    s->ext_rev = SETTINGS_EXT_REV;

    s->stick_deadzone_pct = 6;
    s->stick_outer_pct = 95;
    s->swap_sticks = 0;
    s->gc_trigger_threshold = 120;

    s->gyro_enabled = 1;
    s->gyro_range = GYRO_RANGE_AUTO;
    s->gyro_scale_pct = 100;
    s->accel_scale_pct = 100;

    s->rumble_enabled = 1;
    s->rumble_strength_pct = 100;
    s->rumble_freq_mode = RUMBLE_FREQ_TRANSLATE;
    s->rumble_freq_slope = 117;

    s->usb_report_interval_ms = 8;
    s->led_follow_host = 1;
    s->usb_detach_when_idle = 0;
    s->usb_remote_wakeup = 1;
    s->webusb_enabled = 1;
    s->idle_disconnect_off = 0;
    s->idle_minutes = 15;   // like an Xbox controller
    s->pair_button = platform_has_sync_button() ? 1 : 0;

    s->hotkey_enabled = 1;
    s->wifi_autostart = 1;
    s->wifi_channel = 6;

    default_ssid(s->wifi_ssid, sizeof s->wifi_ssid);
    snprintf(s->wifi_pass, sizeof s->wifi_pass, "switch2pico");

    memset(s->spi_user_cal, 0xFF, sizeof s->spi_user_cal);
}

static uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint16_t clamp_u16(uint16_t v, uint16_t lo, uint16_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void settings_sanitize(settings_t *s) {
    s->webusb_enabled = 1;   // always on (the configuration page needs it)
    for (int t = 0; t < CTRL_TYPE_COUNT; t++) settings_sanitize_profiles((ctrl_type_t)t, &s->prof[t]);
    // Paired controllers: no gaps; the single-controller fields follow the newest.
    int n = 0;
    for (int i = 0; i < BOND_MAX; i++) {
        if (s->bonds[i].used) s->bonds[n++] = s->bonds[i];
    }
    for (int i = n; i < BOND_MAX; i++) memset(&s->bonds[i], 0, sizeof s->bonds[i]);
    settings_bond_sync(s);
    if (s->last_ctrl >= CTRL_TYPE_COUNT) s->last_ctrl = CTRL_PRO;
    if (s->ble_tx_power > 3) s->ble_tx_power = 0;
    s->idle_disconnect_off = s->idle_disconnect_off ? 1 : 0;
    s->idle_minutes = s->idle_minutes ? clamp_u8(s->idle_minutes, 1, 240) : 15;
    s->pair_button = s->pair_button && platform_has_sync_button() ? 1 : 0;
    if (s->gyro_range > GYRO_RANGE_14_3) s->gyro_range = GYRO_RANGE_AUTO;
    s->gyro_scale_pct = clamp_u16(s->gyro_scale_pct, 10, 400);
    s->accel_scale_pct = clamp_u16(s->accel_scale_pct, 10, 400);
    if (s->rumble_freq_mode > RUMBLE_FREQ_FIXED) s->rumble_freq_mode = RUMBLE_FREQ_TRANSLATE;
    s->rumble_freq_slope = clamp_u8(s->rumble_freq_slope, 16, 255);
    s->usb_report_interval_ms = clamp_u8(s->usb_report_interval_ms, 4, 16);
    s->wifi_channel = clamp_u8(s->wifi_channel, 1, 11);
    s->wifi_ssid[sizeof s->wifi_ssid - 1] = 0;
    s->wifi_pass[sizeof s->wifi_pass - 1] = 0;
    if (strlen(s->wifi_ssid) == 0) default_ssid(s->wifi_ssid, sizeof s->wifi_ssid);
    // WPA2 needs 8..63 characters; an empty password means an open network.
    size_t pl = strlen(s->wifi_pass);
    if (pl > 0 && pl < 8) {
        snprintf(s->wifi_pass, sizeof s->wifi_pass, "switch2pico");
    }
}

// Before profiles each controller type had one map per mode, its mode
// shortcut buttons (mode + 1), one USB mode and one set of stick / rumble
// options: they become one profile per mode, the shortcut buttons point at
// the same modes' profiles, and the type's mode is the active profile.
static void profiles_from_legacy(settings_t *s, ctrl_type_t t) {
    bool gc = t == CTRL_GAMECUBE;
    const ctrl_profile_t *g = &s->gc_profile;
    ctrl_profiles_t *c = &s->prof[t];
    memset(c, 0, sizeof *c);
    for (int m = 0; m < USB_MODE_COUNT; m++) {
        profile_t *p = &c->p[m];
        settings_default_profile_for(t, (usb_mode_t)m, p);
        const uint8_t *map = m == USB_MODE_SWITCH_PRO ? (gc ? g->button_map : s->button_map)
                                                      : (gc ? g->mode_map[m] : s->mode_map[m]);
        memcpy(p->map, map, IN_COUNT);
        p->stick_deadzone_pct = gc ? g->stick_deadzone_pct : s->stick_deadzone_pct;
        p->stick_outer_pct = gc ? g->stick_outer_pct : s->stick_outer_pct;
        p->swap_sticks = gc ? g->swap_sticks : s->swap_sticks;
        p->rumble_enabled = gc ? g->rumble_enabled : s->rumble_enabled;
        p->rumble_strength_pct = gc ? g->rumble_strength_pct : s->rumble_strength_pct;
        p->trigger_threshold = s->gc_trigger_threshold;
    }
    memcpy(c->slot, gc ? g->mode_slot : s->mode_slot, MODE_SLOT_COUNT);
    uint8_t mode = gc ? s->gc_usb_mode : s->usb_mode;
    c->active = mode < USB_MODE_COUNT ? mode : 0;
}

static settings_t s_stored;   // last image read from / written to storage

void settings_init(void) {
    memset(&s_stored, 0, sizeof s_stored);
    size_t got = platform_settings_read(&s_stored, sizeof s_stored);
    const settings_t *stored = &s_stored;
    const uint8_t *raw = (const uint8_t *)stored;
    if (got >= offsetof(settings_t, spi_user_cal) && got >= stored->size && stored->magic == SETTINGS_MAGIC && stored->version == SETTINGS_VERSION &&
        stored->size >= offsetof(settings_t, spi_user_cal) + SPI_USER_CAL_SIZE + 4 &&
        stored->size <= sizeof(settings_t) && stored_crc_ok(raw, stored->size)) {
        // Start from defaults so fields added since the save keep sane values.
        settings_defaults(&g_settings);
        memcpy(&g_settings, raw, stored->size - 4u);
        g_settings.size = sizeof g_settings;
        // Fields added after a save hold whatever followed the older layout
        // (padding or nothing): ext_rev says which of them are real.
        uint8_t rev = g_settings.ext_rev;
        if (rev > SETTINGS_EXT_REV) rev = 0;
        if (rev < 1) {
            for (int m = 0; m < MODE_MAP_SLOTS; m++) settings_default_mode_map(CTRL_PRO, (usb_mode_t)m, g_settings.mode_map[m]);
        }
        if (rev < 2) settings_default_mode_slots(g_settings.mode_slot);
        if (rev < 3) g_settings.ble_tx_power = 0;
        if (rev < 4) {
            g_settings.idle_disconnect_off = 0;
            g_settings.idle_minutes = 15;
        }
        if (rev < 5) g_settings.pair_button = platform_has_sync_button() ? 1 : 0;
        if (rev < 6) {
            // The GameCube controller gets its own maps; the Pro's GameCube
            // adapter map (new, GameCube-controller defaults) gets the Pro's.
            settings_default_profile(CTRL_GAMECUBE, &g_settings.gc_profile);
            settings_default_mode_map(CTRL_PRO, USB_MODE_GC_ADAPTER, g_settings.mode_map[USB_MODE_GC_ADAPTER]);
        }
        if (rev < 9) {
            // One USB mode for both before.
            g_settings.gc_usb_mode = g_settings.usb_mode;
            g_settings.last_ctrl = CTRL_PRO;
        }
        if (rev < 8) {
            // One remembered controller before: it becomes the list.
            uint8_t addr[6];
            memcpy(addr, g_settings.ctrl_addr, 6);
            uint8_t had = g_settings.bonded, type = g_settings.ctrl_addr_type;
            uint16_t pid = g_settings.ctrl_pid;
            settings_bond_clear(&g_settings);
            if (had) settings_bond_add(&g_settings, addr, type, pid, NULL);
        }
        if (rev < 7) {
            // GameCube controller sticks / rumble: start from the shared values.
            g_settings.gc_profile.stick_deadzone_pct = g_settings.stick_deadzone_pct;
            g_settings.gc_profile.stick_outer_pct = g_settings.stick_outer_pct;
            g_settings.gc_profile.swap_sticks = g_settings.swap_sticks;
            g_settings.gc_profile.rumble_enabled = g_settings.rumble_enabled;
            g_settings.gc_profile.rumble_strength_pct = g_settings.rumble_strength_pct;
        }
        if (rev < 10) {
            // Profiles: one per mode from that mode's map (after the steps
            // above, which bring the per-mode fields up to date).
            profiles_from_legacy(&g_settings, CTRL_PRO);
            profiles_from_legacy(&g_settings, CTRL_GAMECUBE);
        }
        if (rev < 11) {
            // One profile per button (after the step above made profiles).
            settings_profiles_to_buttons(&g_settings.prof[CTRL_PRO]);
            settings_profiles_to_buttons(&g_settings.prof[CTRL_GAMECUBE]);
        }
        if (rev < 12) {
            // Joy-Con 2 types: new (whatever followed the older layout isn't
            // theirs); the mouse fields of the others were reserved zeros.
            for (int t = CTRL_JOYCON_PAIR; t < CTRL_TYPE_COUNT; t++) {
                settings_default_profiles((ctrl_type_t)t, &g_settings.prof[t]);
            }
        }
        g_settings.ext_rev = SETTINGS_EXT_REV;
        settings_sanitize(&g_settings);
        LOG("settings: loaded from flash (bonded=%d%s)", g_settings.bonded,
            stored->size != sizeof(settings_t) ? ", migrated" : "");
    } else {
        settings_defaults(&g_settings);
        LOG("settings: using defaults");
    }
    s_dirty = false;
}

void settings_save_now(void) {
    g_settings.magic = SETTINGS_MAGIC;
    g_settings.version = SETTINGS_VERSION;
    g_settings.size = sizeof g_settings;
    g_settings.crc = settings_crc(&g_settings);

    if (memcmp(&s_stored, &g_settings, sizeof g_settings) == 0) {
        s_dirty = false;
        return;
    }
    bool ok = platform_settings_write(&g_settings, sizeof g_settings);
    if (ok) s_stored = g_settings;
    s_dirty = false;
    LOG("settings: saved (%s)", ok ? "ok" : "FAILED");
}

void settings_save_later(void) {
    s_dirty = true;
    s_save_deadline = platform_deadline_ms(1500);
}

void settings_task(void) {
    if (s_dirty && platform_time_reached(s_save_deadline)) {
        settings_save_now();
    }
}

void settings_factory_reset(void) {
    settings_defaults(&g_settings);
    settings_save_now();
}
