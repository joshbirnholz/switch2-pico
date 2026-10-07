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

// Settings persistence: one opaque blob of at most `cap` bytes.
// Returns the number of bytes read (0 if nothing stored).
size_t platform_settings_read(void *dst, size_t cap);
bool platform_settings_write(const void *src, size_t len);

// Reboot now, optionally into the UF2 bootloader. Does not return.
void platform_reboot(bool bootloader);

// Diagnostic output (debug UART / serial), one line at a time.
void platform_log_output(const char *line);

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
