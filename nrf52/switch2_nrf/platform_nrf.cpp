// Platform services for nRF52840 boards running the Adafruit nRF52 core
// (see core/src/platform.h).

#include <Adafruit_LittleFS.h>
#include <Arduino.h>
#include <InternalFileSystem.h>
#include <bluefruit.h>

#include "log.h"
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
    platform_log_flush();   // keep the log up to the restart
    if (bootloader) {
        enterUf2Dfu();   // Adafruit bootloader: mass storage UF2 mode
    }
    NVIC_SystemReset();
    for (;;) {
    }
}

// ---------------------------------------------------------------------------
// Watchdog and reset diagnostics
// ---------------------------------------------------------------------------
// The WDT raises its TIMEOUT interrupt two 32 kHz cycles before it resets the
// chip. The handler stores the interrupted PC/LR in RAM that isn't zeroed at
// boot, so the next boot can log where the firmware was stuck (look the
// address up with arm-none-eabi-addr2line -e switch2_nrf.ino.elf). Priority 2
// is the highest an application may use alongside the SoftDevice; a hang with
// interrupts masked therefore leaves no PC, which is itself a clue.
struct hang_frame_t {
    uint32_t pc, lr, magic;
};
#define HANG_MAGIC 0x48414E47u   // "HANG"
__attribute__((section(".noinit"))) volatile hang_frame_t g_s2p_hang;

extern "C" __attribute__((naked)) void WDT_IRQHandler(void) {
    __asm volatile(
        "tst lr, #4          \n"
        "ite eq              \n"
        "mrseq r0, msp       \n"
        "mrsne r0, psp       \n"
        "ldr r1, [r0, #24]   \n"   // stacked PC
        "ldr r3, [r0, #20]   \n"   // stacked LR
        "ldr r2, =g_s2p_hang \n"
        "str r1, [r2, #0]    \n"
        "str r3, [r2, #4]    \n"
        "ldr r1, =0x48414E47 \n"
        "str r1, [r2, #8]    \n"
        "b .                 \n"   // the reset follows within microseconds
        ".ltorg              \n");
}

void platform_watchdog_start(void) {
    if (!NRF_WDT->RUNSTATUS) {
        NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos) | (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
        NRF_WDT->CRV = 8UL * 32768UL - 1;   // ~8 s
        NRF_WDT->RREN = WDT_RREN_RR0_Msk;
        NRF_WDT->INTENSET = WDT_INTENSET_TIMEOUT_Msk;
        NVIC_SetPriority(WDT_IRQn, 2);
        NVIC_ClearPendingIRQ(WDT_IRQn);
        NVIC_EnableIRQ(WDT_IRQn);
        NRF_WDT->TASKS_START = 1;
    }
    NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}

void platform_watchdog_feed(void) {
    NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}

void platform_log_reset_reason(void) {
    uint32_t r = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = r;   // write-1-to-clear, so the next boot sees fresh bits
    const char *why = r & POWER_RESETREAS_DOG_Msk      ? "watchdog (main loop stuck)"
                    : r & POWER_RESETREAS_LOCKUP_Msk   ? "CPU lockup"
                    : r & POWER_RESETREAS_SREQ_Msk     ? "software (reboot or crash)"
                    : r & POWER_RESETREAS_RESETPIN_Msk ? "reset button"
                    : r & POWER_RESETREAS_VBUS_Msk     ? "USB power"
                    : r ? "other" : "power on";
    LOG("boot: last reset: %s (RESETREAS 0x%08lx)", why, (unsigned long)r);
    if ((r & POWER_RESETREAS_DOG_Msk) && g_s2p_hang.magic == HANG_MAGIC) {
        LOG("boot: stuck at pc=0x%08lx lr=0x%08lx", (unsigned long)g_s2p_hang.pc, (unsigned long)g_s2p_hang.lr);
    }
    g_s2p_hang.magic = 0;
}

void platform_journal_append(const char *line, size_t len);
void platform_journal_flush(void);

void platform_log_output(const char *line) {
#ifdef S2P_UART_LOG
    Serial1.print(line);
#endif
    platform_journal_append(line, strlen(line));
}

void platform_log_flush(void) {
    platform_journal_flush();
}

}  // extern "C"
