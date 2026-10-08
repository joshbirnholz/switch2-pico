// Firmware update without the UF2 drive, for nRF52840 boards with the Adafruit
// UF2 bootloader (see platform_fw_* in core/src/platform.h).
//
// Flash map (S140 v6): application 0x26000-0xED000, InternalFS (settings)
// 0xED000-0xF4000, bootloader 0xF4000-, bootloader settings page 0xFF000.
// The new image is staged at FW_STAGE (above the running image, below
// InternalFS) and described by a record in the page FW_META. At boot,
// before the SoftDevice is enabled, the image is re-verified and copied to
// 0x26000 by code running from RAM:
//   - the first page (vector table) is erased first and written last, its
//     first word very last: an interrupted copy leaves 0xFFFFFFFF there, and
//     the bootloader then stays in UF2 mode instead of starting a broken app;
//   - the record is cleared only after the copy, so a reset during the copy
//     simply repeats it;
//   - InternalFS, the bootloader and the SoftDevice are never touched; the
//     bootloader settings page is rewritten only if it still holds an app CRC
//     from a serial (nrfutil) flash, which would no longer match.

#include <Arduino.h>
#include <string.h>

#include <InternalFileSystem.h>   // makes the library (and its flash layer) available
#include "flash/flash_nrf5x.h"
#include "platform.h"

#define FW_APP_BASE   0x26000UL
#define FW_STAGE      0x86000UL
#define FW_META       0xEC000UL
#define FW_MAX        (FW_STAGE - FW_APP_BASE)   // copy target never overlaps the staged image
#define FW_PAGE       4096UL
#define BL_SETTINGS   0xFF000UL
#define FW_MAGIC      0x46575550UL               // "FWUP"
#define UF2_FAMILY    0xADA52840UL

// 0xE8000-0xEBFFF holds the saved log (log_journal_nrf.cpp).
static_assert(FW_STAGE + FW_MAX <= 0xE8000UL, "update staging overlaps the saved log");

struct fw_meta_t {
    uint32_t magic, size, crc, check;
};

static uint32_t meta_check(const fw_meta_t *m) {
    return m->magic ^ m->size ^ m->crc ^ 0x5A5A5A5AUL;
}

static uint32_t crc32_update(uint32_t crc, const volatile uint8_t *p, uint32_t n) {
    while (n--) {
        crc ^= *p++;
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1)));
    }
    return crc;
}

static uint32_t crc32_flash(uint32_t addr, uint32_t len) {
    return ~crc32_update(0xFFFFFFFFUL, (const volatile uint8_t *)addr, len);
}

// A Cortex-M image starts with the initial stack pointer (in RAM) and the
// reset handler (a Thumb address inside the image as it will run).
static bool vectors_ok(uint32_t staged, uint32_t size) {
    const volatile uint32_t *v = (const volatile uint32_t *)staged;
    uint32_t sp = v[0], reset = v[1];
    return sp >= 0x20000000UL && sp <= 0x20040000UL && (reset & 1) && reset >= FW_APP_BASE &&
           reset < FW_APP_BASE + size;
}

extern "C" {

extern uint32_t __etext, __data_start__, __data_end__;

void platform_fw_info(platform_fw_info_t *out) {
    out->uf2_family = UF2_FAMILY;
    out->base = FW_APP_BASE;
    out->max_size = FW_MAX;
}

static uint32_t s_size, s_crc, s_next;
static bool s_active;

bool platform_fw_begin(uint32_t size, uint32_t crc, const char **err) {
    s_active = false;
    // The running image must end below the staging area.
    uint32_t used_end = (uint32_t)&__etext + ((uint32_t)&__data_end__ - (uint32_t)&__data_start__);
    if (used_end > FW_STAGE) {
        *err = "firmware too large to stage an update";
        return false;
    }
    if (size < 1024 || size > FW_MAX) {
        *err = "image size out of range";
        return false;
    }
    // Drop any earlier pending update first.
    const fw_meta_t *m = (const fw_meta_t *)FW_META;
    if (m->magic != 0xFFFFFFFFUL) {
        flash_nrf5x_erase(FW_META);
        flash_nrf5x_flush();
    }
    s_size = size;
    s_crc = crc;
    s_next = 0;
    s_active = true;
    return true;
}

bool platform_fw_write(uint32_t off, const uint8_t *data, uint32_t len, const char **err) {
    if (!s_active) {
        *err = "no update in progress";
        return false;
    }
    if (off != s_next || off + len > s_size || len == 0) {
        *err = "chunk out of order";
        return false;
    }
    if (flash_nrf5x_write(FW_STAGE + off, data, len) != (int)len) {
        *err = "flash write failed";
        s_active = false;
        return false;
    }
    s_next += len;
    return true;
}

bool platform_fw_finish(const char **err) {
    if (!s_active || s_next != s_size) {
        *err = "image incomplete";
        return false;
    }
    s_active = false;
    flash_nrf5x_flush();
    if (crc32_flash(FW_STAGE, s_size) != s_crc) {
        *err = "staged image CRC mismatch";
        return false;
    }
    if (!vectors_ok(FW_STAGE, s_size)) {
        *err = "not an nRF52840 application image";
        return false;
    }
    fw_meta_t m = {FW_MAGIC, s_size, s_crc, 0};
    m.check = meta_check(&m);
    flash_nrf5x_write(FW_META, &m, sizeof m);
    flash_nrf5x_flush();
    return true;
}

// ---- Boot-time copy (runs from RAM, SoftDevice off, interrupts off) --------
#define RAMFUNC __attribute__((section(".data.fw_apply"), noinline, long_call))

// A macro, not a function: nothing called during the copy may live in flash.
#define nvmc_wait()             \
    do {                        \
        while (!NRF_NVMC->READY) { \
        }                       \
    } while (0)

RAMFUNC static void ram_erase(uint32_t page) {
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
    nvmc_wait();
    NRF_NVMC->ERASEPAGE = page;
    nvmc_wait();
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    nvmc_wait();
}

RAMFUNC static void ram_write(uint32_t dst, const volatile uint32_t *src, uint32_t words) {
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
    nvmc_wait();
    for (uint32_t i = 0; i < words; i++) {
        ((volatile uint32_t *)dst)[i] = src[i];
        nvmc_wait();
    }
    NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
    nvmc_wait();
}

RAMFUNC static void ram_apply(uint32_t size) {
    const uint32_t words = (size + 3) / 4;
    const uint32_t pages = (size + FW_PAGE - 1) / FW_PAGE;
    // 1. Vector table page out first: from now on an interruption leaves no valid app.
    ram_erase(FW_APP_BASE);
    // 2. Everything after the first page.
    for (uint32_t p = 1; p < pages; p++) {
        NRF_WDT->RR[0] = WDT_RR_RR_Reload;
        uint32_t dst = FW_APP_BASE + p * FW_PAGE;
        ram_erase(dst);
        uint32_t w = words - p * (FW_PAGE / 4);
        if (w > FW_PAGE / 4) w = FW_PAGE / 4;
        ram_write(dst, (const volatile uint32_t *)(FW_STAGE + p * FW_PAGE), w);
    }
    // 3. The first page, its first word (initial stack pointer) last.
    uint32_t w0 = words < FW_PAGE / 4 ? words : FW_PAGE / 4;
    ram_write(FW_APP_BASE + 4, (const volatile uint32_t *)(FW_STAGE + 4), w0 - 1);
    ram_write(FW_APP_BASE, (const volatile uint32_t *)FW_STAGE, 1);
    // 4. Bootloader settings: "valid app, no CRC" (what a UF2 copy leaves), if needed.
    volatile uint32_t *bl = (volatile uint32_t *)BL_SETTINGS;
    if (bl[0] != 0x00000001UL) {
        // On the stack: constants in flash belong to the image just replaced.
        volatile uint32_t valid_no_crc = 0x00000001UL;   // bank_0 = valid app, bank_0_crc = 0
        ram_erase(BL_SETTINGS);
        ram_write(BL_SETTINGS, &valid_no_crc, 1);
    }
    // 5. Done: forget the pending update and restart into the new firmware.
    ram_erase(FW_META);
    __DSB();
    SCB->AIRCR = (0x5FAUL << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk;
    __DSB();
    for (;;) {
    }
}

void platform_fw_apply_if_pending(void) {
    const fw_meta_t *m = (const fw_meta_t *)FW_META;
    if (m->magic != FW_MAGIC) return;
    uint8_t sd = 0;
    sd_softdevice_is_enabled(&sd);
    if (sd) return;   // must run before Bluefruit.begin()
    bool valid = m->check == meta_check(m) && m->size >= 1024 && m->size <= FW_MAX &&
                 crc32_flash(FW_STAGE, m->size) == m->crc && vectors_ok(FW_STAGE, m->size);
    if (!valid) {
        // Stale or damaged record: discard it, keep the running firmware.
        __disable_irq();
        ram_erase(FW_META);
        __enable_irq();
        return;
    }
    digitalWrite(LED_BUILTIN, HIGH);
    NRF_POWER->GPREGRET2 = 0xA6;   // a planned restart (see platform_nrf.cpp)
    __disable_irq();
    ram_apply(m->size);   // does not return
}

}  // extern "C"
