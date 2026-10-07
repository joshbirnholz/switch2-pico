// Platform services for nRF52840 boards running the Adafruit nRF52 core
// (see core/src/platform.h).

#include <Adafruit_LittleFS.h>
#include <Arduino.h>
#include <InternalFileSystem.h>
#include <bluefruit.h>

#include "platform.h"

using namespace Adafruit_LittleFS_Namespace;

#define SETTINGS_FILE "/s2p_settings.bin"

extern "C" {

uint32_t platform_millis(void) {
    return millis();
}

uint32_t platform_random32(void) {
    uint32_t v = 0;
    // The SoftDevice owns the RNG peripheral once Bluefruit is running.
    if (sd_rand_application_vector_get((uint8_t *)&v, sizeof v) != NRF_SUCCESS) {
        v = (uint32_t)micros() * 2654435761u ^ NRF_FICR->DEVICEID[0];
    }
    return v;
}

void platform_unique_id(uint8_t out[PLATFORM_UNIQUE_ID_LEN]) {
    uint32_t a = NRF_FICR->DEVICEID[0], b = NRF_FICR->DEVICEID[1];
    memcpy(out, &a, 4);
    memcpy(out + 4, &b, 4);
}

const char *platform_name(void) {
    return "nrf52840";
}

bool platform_has_wifi(void) {
    return false;
}

size_t platform_settings_read(void *dst, size_t cap) {
    File f(InternalFS);
    if (!f.open(SETTINGS_FILE, FILE_O_READ)) return 0;
    size_t n = f.read(dst, cap);
    f.close();
    return n;
}

bool platform_settings_write(const void *src, size_t len) {
    InternalFS.remove(SETTINGS_FILE);
    File f(InternalFS);
    if (!f.open(SETTINGS_FILE, FILE_O_WRITE)) return false;
    size_t n = f.write((const uint8_t *)src, len);
    f.close();
    return n == len;
}

void platform_reboot(bool bootloader) {
    if (bootloader) {
        enterUf2Dfu();   // Adafruit bootloader: mass storage UF2 mode
    }
    NVIC_SystemReset();
    for (;;) {
    }
}

void platform_log_output(const char *line) {
#ifdef S2P_UART_LOG
    Serial1.print(line);
#else
    (void)line;
#endif
}

}  // extern "C"
