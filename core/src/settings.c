#include "settings.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "platform.h"

settings_t g_settings;

_Static_assert(sizeof(settings_t) <= 1024, "settings_t too large");

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

void settings_defaults(settings_t *s) {
    memset(s, 0, sizeof *s);
    s->magic = SETTINGS_MAGIC;
    s->version = SETTINGS_VERSION;
    s->size = sizeof *s;

    // Identity mapping for every button the Switch 1 Pro Controller also has.
    for (int i = IN_A; i <= IN_RIGHT; i++) {
        s->button_map[i] = (uint8_t)(OUT_A + (i - IN_A));
    }
    // The Switch 2 only buttons default to the stick clicks (like most back
    // paddle setups) and C is unassigned.
    s->button_map[IN_GL] = OUT_LSTICK;
    s->button_map[IN_GR] = OUT_RSTICK;
    s->button_map[IN_C] = OUT_NONE;

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

    s->hotkey_enabled = 1;
    s->wifi_autostart = 1;
    s->wifi_channel = 6;

    uint8_t id[PLATFORM_UNIQUE_ID_LEN];
    platform_unique_id(id);
    snprintf(s->wifi_ssid, sizeof s->wifi_ssid, "Switch2-Pico-%02X%02X", id[PLATFORM_UNIQUE_ID_LEN - 2],
             id[PLATFORM_UNIQUE_ID_LEN - 1]);
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
    for (int i = 0; i < IN_COUNT; i++) {
        if (s->button_map[i] >= OUT_COUNT) s->button_map[i] = OUT_NONE;
    if (s->usb_mode >= USB_MODE_COUNT) s->usb_mode = USB_MODE_SWITCH_PRO;
    }
    s->stick_deadzone_pct = clamp_u8(s->stick_deadzone_pct, 0, 40);
    s->stick_outer_pct = clamp_u8(s->stick_outer_pct, 50, 100);
    s->swap_sticks = s->swap_sticks ? 1 : 0;
    if (s->gyro_range > GYRO_RANGE_14_3) s->gyro_range = GYRO_RANGE_AUTO;
    s->gyro_scale_pct = clamp_u16(s->gyro_scale_pct, 10, 400);
    s->accel_scale_pct = clamp_u16(s->accel_scale_pct, 10, 400);
    s->rumble_strength_pct = clamp_u8(s->rumble_strength_pct, 0, 200);
    if (s->rumble_freq_mode > RUMBLE_FREQ_FIXED) s->rumble_freq_mode = RUMBLE_FREQ_TRANSLATE;
    s->rumble_freq_slope = clamp_u8(s->rumble_freq_slope, 16, 255);
    s->usb_report_interval_ms = clamp_u8(s->usb_report_interval_ms, 4, 16);
    s->wifi_channel = clamp_u8(s->wifi_channel, 1, 11);
    s->wifi_ssid[sizeof s->wifi_ssid - 1] = 0;
    s->wifi_pass[sizeof s->wifi_pass - 1] = 0;
    if (strlen(s->wifi_ssid) == 0) {
        settings_t d;
        settings_defaults(&d);
        memcpy(s->wifi_ssid, d.wifi_ssid, sizeof s->wifi_ssid);
    }
    // WPA2 needs 8..63 characters; an empty password means an open network.
    size_t pl = strlen(s->wifi_pass);
    if (pl > 0 && pl < 8) {
        snprintf(s->wifi_pass, sizeof s->wifi_pass, "switch2pico");
    }
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
