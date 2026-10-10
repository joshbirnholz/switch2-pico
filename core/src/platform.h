#ifndef S2P_PLATFORM_H
#define S2P_PLATFORM_H

// Board services used by the shared core. Implemented once per target
// (pico/platform_pico.c, nrf52/switch2_nrf/platform_nrf.cpp).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLATFORM_UNIQUE_ID_LEN 8

uint32_t platform_millis(void);
uint32_t platform_random32(void);
void platform_unique_id(uint8_t out[PLATFORM_UNIQUE_ID_LEN]);
// Short board name reported to the configuration page ("pico2w", "nrf52840").
const char *platform_name(void);
// True if this board has the Wi-Fi configuration access point.
bool platform_has_wifi(void);
// A button on the board that can act as a Sync button (Pico: BOOTSEL).
bool platform_has_sync_button(void);

// Settings persistence: one opaque blob of at most `cap` bytes.
// Returns the number of bytes read (0 if nothing stored).
size_t platform_settings_read(void *dst, size_t cap);
bool platform_settings_write(const void *src, size_t len);

// Reboot now, optionally into the UF2 bootloader. Does not return.
void platform_reboot(bool bootloader);

// Diagnostic output (debug UART / serial), one line at a time.
void platform_log_output(const char *line);

// Log kept in flash across resets and power loss, where the board has one
// (nRF52840; elsewhere these do nothing). Write out pending lines now; copy
// the newest `cap` bytes saved before this boot.
void platform_log_flush(void);

// Supply voltages in mV, where the board can measure them (nRF52840): the
// chip's own supply (VDD, ~3.3 V) and the USB input (VBUS, ~5 V), now and
// the lowest since start. False elsewhere.
typedef struct {
    uint16_t vdd, vbus, vdd_min, vbus_min;
} platform_supply_t;
bool platform_supply(platform_supply_t *out);
// USB power (VBUS) present: 1 yes, 0 no, -1 unknown.
int platform_usb_power(void);
// The USB descriptors changed (usb_mode_set_config_only()); called while
// disconnected. Boards that build them once rebuild them here.
void platform_usb_rebuild(void);
// Whether the board shows the configuration-only device while no controller
// is connected (otherwise it stays the emulated controller, whose
// configuration interface the page and the Decky plugin use as well).
bool platform_usb_config_only_supported(void);
size_t platform_saved_log(char *dst, size_t cap);

// Firmware update over the configuration channel (WebUSB / HTTP), without
// the UF2 drive. The page sends the raw application image (extracted from
// the .uf2) in order; the board stages it in flash it doesn't use, checks it
// and records it as pending. On the next boot, before anything else,
// platform_fw_apply_if_pending() copies it over the running firmware. Every
// step leaves either the old firmware, the new one, or no valid app (the UF2
// bootloader then keeps the board, for drag-and-drop recovery).
typedef struct {
    uint32_t uf2_family;   // UF2 family ID of this board's images
    uint32_t base;         // flash address the image starts at
    uint32_t max_size;     // largest image that can be staged
} platform_fw_info_t;
void platform_fw_info(platform_fw_info_t *out);
bool platform_fw_begin(uint32_t size, uint32_t crc32, const char **err);
// Sequential chunks: `off` must continue where the previous one ended.
bool platform_fw_write(uint32_t off, const uint8_t *data, uint32_t len, const char **err);
// Checks the staged CRC and image header, then marks the update pending.
bool platform_fw_finish(const char **err);
void platform_fw_apply_if_pending(void);

// Watchdog: started by the board's startup code, fed from app_core_task().
// If the main loop stops for ~8 s the board resets (and logs why next boot).
void platform_watchdog_feed(void);

// Wrap-safe millisecond deadline helpers.
static inline bool platform_time_reached(uint32_t deadline) {
    return (int32_t)(platform_millis() - deadline) >= 0;
}
static inline uint32_t platform_deadline_ms(uint32_t ms) {
    return platform_millis() + ms;
}

#ifdef __cplusplus
}
#endif

#endif
