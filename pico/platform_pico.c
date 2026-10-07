// Platform services for the Raspberry Pi Pico W / Pico 2 W (see core/src/platform.h).

#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/rand.h"
#include "pico/time.h"
#include "pico/unique_id.h"

#include "platform.h"

// One flash sector for settings, well clear of the BTstack TLV bank at the
// very end of flash.
#define SETTINGS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - 16 * FLASH_SECTOR_SIZE)
#define SETTINGS_MAX 1024

uint32_t platform_millis(void) {
    return to_ms_since_boot(get_absolute_time());
}

uint32_t platform_random32(void) {
    return get_rand_32();
}

void platform_unique_id(uint8_t out[PLATFORM_UNIQUE_ID_LEN]) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    memcpy(out, id.id, PLATFORM_UNIQUE_ID_LEN);
}

const char *platform_name(void) {
#if PICO_RP2040
    return "picow";
#else
    return "pico2w";
#endif
}

bool platform_has_wifi(void) {
    return true;
}

size_t platform_settings_read(void *dst, size_t cap) {
    if (cap > SETTINGS_MAX) cap = SETTINGS_MAX;
    memcpy(dst, (const void *)(XIP_BASE + SETTINGS_FLASH_OFFSET), cap);
    return cap;
}

static uint8_t s_page_buf[SETTINGS_MAX];

static void flash_write_cb(void *param) {
    (void)param;
    flash_range_erase(SETTINGS_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SETTINGS_FLASH_OFFSET, s_page_buf, sizeof s_page_buf);
}

bool platform_settings_write(const void *src, size_t len) {
    if (len > sizeof s_page_buf) return false;
    memset(s_page_buf, 0xFF, sizeof s_page_buf);
    memcpy(s_page_buf, src, len);
    return flash_safe_execute(flash_write_cb, NULL, 500) == PICO_OK;
}

void platform_log_output(const char *line) {
    fputs(line, stdout);
}
