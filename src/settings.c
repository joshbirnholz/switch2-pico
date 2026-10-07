#include "settings.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/time.h"
#include "pico/unique_id.h"

#include "log.h"

settings_t g_settings;

// One flash sector, well clear of the BTstack TLV bank at the very end of flash.
#define SETTINGS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 16 * FLASH_SECTOR_SIZE)

static_assert(sizeof(settings_t) <= FLASH_SECTOR_SIZE, "settings_t too large");

static bool s_dirty;
static absolute_time_t s_save_deadline;

static const char *const IN_NAMES[IN_COUNT] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Minus", "Plus", "LStick", "RStick",
    "Home", "Capture", "Up", "Down", "Left", "Right", "GL", "GR", "C",
};

static const char *const OUT_NAMES[OUT_COUNT] = {
    "None", "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Minus", "Plus", "LStick", "RStick",
    "Home", "Capture", "Up", "Down", "Left", "Right",
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

    s->nfc_enabled = 1;
    s->nfc_report_in_descriptor = 1;

    s->usb_report_interval_ms = 8;
    s->led_follow_host = 1;
    s->usb_detach_when_idle = 0;
    s->usb_remote_wakeup = 1;

    s->hotkey_enabled = 1;
    s->wifi_autostart = 1;
    s->wifi_channel = 6;

    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    snprintf(s->wifi_ssid, sizeof s->wifi_ssid, "Switch2-Pico-%02X%02X",
             id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2], id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);
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

void settings_init(void) {
    const settings_t *stored = (const settings_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET);
    if (stored->magic == SETTINGS_MAGIC && stored->version == SETTINGS_VERSION &&
        stored->size == sizeof(settings_t) && stored->crc == settings_crc(stored)) {
        memcpy(&g_settings, stored, sizeof g_settings);
        settings_sanitize(&g_settings);
        LOG("settings: loaded from flash (bonded=%d)", g_settings.bonded);
    } else {
        settings_defaults(&g_settings);
        LOG("settings: using defaults");
    }
    s_dirty = false;
}

static void flash_write_cb(void *param) {
    const uint8_t *page_data = (const uint8_t *)param;
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, page_data,
                        (sizeof(settings_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE);
}

void settings_save_now(void) {
    static uint8_t buf[(sizeof(settings_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE];
    g_settings.magic = SETTINGS_MAGIC;
    g_settings.version = SETTINGS_VERSION;
    g_settings.size = sizeof g_settings;
    g_settings.crc = settings_crc(&g_settings);

    const settings_t *stored = (const settings_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET);
    if (memcmp(stored, &g_settings, sizeof g_settings) == 0) {
        s_dirty = false;
        return;
    }
    memset(buf, 0xFF, sizeof buf);
    memcpy(buf, &g_settings, sizeof g_settings);
    int rc = flash_safe_execute(flash_write_cb, buf, 500);
    s_dirty = false;
    LOG("settings: saved to flash (rc=%d)", rc);
}

void settings_save_later(void) {
    s_dirty = true;
    s_save_deadline = make_timeout_time_ms(1500);
}

void settings_task(void) {
    if (s_dirty && time_reached(s_save_deadline)) {
        settings_save_now();
    }
}

void settings_factory_reset(void) {
    settings_defaults(&g_settings);
    settings_save_now();
}
