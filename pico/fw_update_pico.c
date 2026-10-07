// Firmware update without the UF2 drive, for the Pico W / Pico 2 W (see
// platform_fw_* in core/src/platform.h).
//
// The new image is staged in the second half of flash (FW_STAGE) and described
// by a record in the sector just below the settings area (FW_META). At boot,
// before anything else, the image is re-verified and copied to the start of
// flash by code running from RAM. Sector 0 (the boot block / vector table) is
// erased first and programmed last, with its first 256 bytes very last: an
// interrupted copy leaves no bootable image, and the boot ROM then starts the
// USB (BOOTSEL) bootloader instead of a broken app. The record is cleared only
// after the copy, so a reset during it repeats the copy.

#include <string.h>

#include "hardware/flash.h"
#include "hardware/structs/psm.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "pico/flash.h"

#include "platform.h"

#define FW_STAGE   0x200000u                                        // flash offset
#define FW_META    (PICO_FLASH_SIZE_BYTES - 17 * FLASH_SECTOR_SIZE)  // below settings (-16 sectors)
#define FW_MAX     0x1C0000u                                        // copy target ends below FW_STAGE
#define FW_MAGIC   0x46575550u                                      // "FWUP"
#if PICO_RP2040
#define UF2_FAMILY 0xE48BFF56u
#define VECTOR_OFF 0x100u    // after the 256 byte boot2 block
#else
#define UF2_FAMILY 0xE48BFF59u   // RP2350, ARM secure
#define VECTOR_OFF 0x000u
#endif

_Static_assert(FW_STAGE + FW_MAX <= FW_META, "staging area overlaps the update record");

typedef struct {
    uint32_t magic, size, crc, check;
} fw_meta_t;

static uint32_t meta_check(const fw_meta_t *m) {
    return m->magic ^ m->size ^ m->crc ^ 0x5A5A5A5Au;
}

static uint32_t crc32_xip(uint32_t off, uint32_t len) {
    const volatile uint8_t *p = (const volatile uint8_t *)(XIP_BASE + off);
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static bool vectors_ok(uint32_t staged_off, uint32_t size) {
    const volatile uint32_t *v = (const volatile uint32_t *)(XIP_BASE + staged_off + VECTOR_OFF);
    uint32_t sp = v[0], reset = v[1];
    return sp >= 0x20000000u && sp <= 0x20082000u && (reset & 1) && reset >= XIP_BASE &&
           reset < XIP_BASE + size;
}

void platform_fw_info(platform_fw_info_t *out) {
    out->uf2_family = UF2_FAMILY;
    out->base = XIP_BASE;
    out->max_size = FW_MAX;
}

// ---- Staging (normal operation) ---------------------------------------------
static uint8_t s_sector[FLASH_SECTOR_SIZE];
static uint32_t s_size, s_crc, s_next, s_sector_off;
static bool s_active;

typedef struct {
    uint32_t off;
    const uint8_t *data;
    uint32_t len;     // multiple of FLASH_PAGE_SIZE
} flash_op_t;

static void erase_program_cb(void *param) {
    const flash_op_t *op = param;
    flash_range_erase(op->off, FLASH_SECTOR_SIZE);
    if (op->len) flash_range_program(op->off, op->data, op->len);
}

static bool erase_program(uint32_t off, const uint8_t *data, uint32_t len) {
    flash_op_t op = {off, data, len};
    return flash_safe_execute(erase_program_cb, &op, 1000) == PICO_OK;
}

static bool flush_sector(void) {
    uint32_t used = s_next - s_sector_off;   // bytes of this sector received
    if (!used) return true;
    uint32_t len = (used + FLASH_PAGE_SIZE - 1) & ~(FLASH_PAGE_SIZE - 1);
    memset(s_sector + used, 0xFF, sizeof s_sector - used);
    bool ok = erase_program(FW_STAGE + s_sector_off, s_sector, len);
    s_sector_off = s_next;
    return ok;
}

extern char __flash_binary_end;

bool platform_fw_begin(uint32_t size, uint32_t crc, const char **err) {
    s_active = false;
    if ((uint32_t)&__flash_binary_end - XIP_BASE > FW_STAGE) {
        *err = "firmware too large to stage an update";
        return false;
    }
    if (size < 1024 || size > FW_MAX) {
        *err = "image size out of range";
        return false;
    }
    const fw_meta_t *m = (const fw_meta_t *)(XIP_BASE + FW_META);
    if (m->magic != 0xFFFFFFFFu && !erase_program(FW_META, NULL, 0)) {
        *err = "flash erase failed";
        return false;
    }
    s_size = size;
    s_crc = crc;
    s_next = s_sector_off = 0;
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
    while (len) {
        uint32_t in_sector = s_next - s_sector_off;
        uint32_t n = FLASH_SECTOR_SIZE - in_sector;
        if (n > len) n = len;
        memcpy(s_sector + in_sector, data, n);
        s_next += n;
        data += n;
        len -= n;
        if (s_next - s_sector_off == FLASH_SECTOR_SIZE && !flush_sector()) {
            *err = "flash write failed";
            s_active = false;
            return false;
        }
    }
    return true;
}

bool platform_fw_finish(const char **err) {
    if (!s_active || s_next != s_size) {
        *err = "image incomplete";
        return false;
    }
    s_active = false;
    if (!flush_sector()) {
        *err = "flash write failed";
        return false;
    }
    if (crc32_xip(FW_STAGE, s_size) != s_crc) {
        *err = "staged image CRC mismatch";
        return false;
    }
    if (!vectors_ok(FW_STAGE, s_size)) {
        *err = "not an application image for this board";
        return false;
    }
    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    fw_meta_t m = {FW_MAGIC, s_size, s_crc, 0};
    m.check = meta_check(&m);
    memcpy(page, &m, sizeof m);
    if (!erase_program(FW_META, page, sizeof page)) {
        *err = "flash write failed";
        return false;
    }
    return true;
}

// ---- Boot-time copy (RAM only, interrupts off) -------------------------------
static uint8_t s_copy[FLASH_SECTOR_SIZE];

static void __no_inline_not_in_flash_func(ram_copy_from_xip)(uint32_t off) {
    const volatile uint32_t *src = (const volatile uint32_t *)(XIP_BASE + off);
    uint32_t *dst = (uint32_t *)s_copy;
    for (uint32_t i = 0; i < FLASH_SECTOR_SIZE / 4; i++) dst[i] = src[i];
}

static void __no_inline_not_in_flash_func(ram_apply)(uint32_t size) {
    const uint32_t sectors = (size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
    // 1. Sector 0 out first: from now on an interruption leaves nothing bootable.
    flash_range_erase(0, FLASH_SECTOR_SIZE);
    // 2. Everything after sector 0.
    for (uint32_t s = 1; s < sectors; s++) {
        ram_copy_from_xip(FW_STAGE + s * FLASH_SECTOR_SIZE);
        flash_range_erase(s * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
        flash_range_program(s * FLASH_SECTOR_SIZE, s_copy, FLASH_SECTOR_SIZE);
    }
    // 3. Sector 0, its first page very last.
    ram_copy_from_xip(FW_STAGE);
    flash_range_program(FLASH_PAGE_SIZE, s_copy + FLASH_PAGE_SIZE, FLASH_SECTOR_SIZE - FLASH_PAGE_SIZE);
    flash_range_program(0, s_copy, FLASH_PAGE_SIZE);
    // 4. Forget the pending update and restart into the new firmware.
    flash_range_erase(FW_META, FLASH_SECTOR_SIZE);
    psm_hw->wdsel = PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS);
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
    for (;;) {
    }
}

void platform_fw_apply_if_pending(void) {
    const fw_meta_t *m = (const fw_meta_t *)(XIP_BASE + FW_META);
    if (m->magic != FW_MAGIC) return;
    bool valid = m->check == meta_check(m) && m->size >= 1024 && m->size <= FW_MAX &&
                 crc32_xip(FW_STAGE, m->size) == m->crc && vectors_ok(FW_STAGE, m->size);
    uint32_t irq = save_and_disable_interrupts();
    if (!valid) {
        // Stale or damaged record: discard it, keep the running firmware.
        flash_range_erase(FW_META, FLASH_SECTOR_SIZE);
        restore_interrupts(irq);
        return;
    }
    ram_apply(m->size);   // does not return
}
