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

// Restart markers in GPREGRET2 (see platform_log_reset_reason).
#define ALIVE_MARK   0xA5u   // running
#define PLANNED_MARK 0xA6u   // restarting on purpose (update, mode change, reboot)
extern "C" void platform_set_restart_mark(uint8_t mark);

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
    platform_set_restart_mark(PLANNED_MARK);
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

// Supply voltages, sampled every 250 ms with the SAADC (10-bit, 0.6 V
// reference, gain 1/6: full scale 3.6 V; VBUS through the internal /5
// divider). A new low below 3.0 V (VDD) or 4.5 V (VBUS) is logged, so a
// supply that sags before a power loss shows in the saved log.
static platform_supply_t s_sup;

static void supply_task(void) {
    static uint32_t next;
    if ((int32_t)(millis() - next) < 0) return;
    next = millis() + 250;
    uint16_t vdd = (uint16_t)(analogReadVDD() * 3600UL / 1023);
    uint16_t vbus = (uint16_t)(analogReadVDDHDIV5() * 3600UL * 5 / 1023);
    s_sup.vdd = vdd;
    s_sup.vbus = vbus;
    if (!s_sup.vdd_min || vdd < s_sup.vdd_min) {
        if (s_sup.vdd_min && vdd < 3000 && s_sup.vdd_min - vdd >= 50) LOG("power: chip supply dropped to %u mV", vdd);
        s_sup.vdd_min = vdd;
    }
    if (!s_sup.vbus_min || vbus < s_sup.vbus_min) {
        if (s_sup.vbus_min && vbus < 4500 && s_sup.vbus_min - vbus >= 100) LOG("power: USB supply dropped to %u mV", vbus);
        s_sup.vbus_min = vbus;
    }
}

int platform_usb_power(void) {
    uint32_t st = 0;
    if (sd_power_usbregstatus_get(&st) != NRF_SUCCESS) return -1;
    return (st & POWER_USBREGSTATUS_VBUSDETECT_Msk) ? 1 : 0;
}

bool platform_supply(platform_supply_t *out) {
    if (!s_sup.vdd) return false;
    *out = s_sup;
    return true;
}

// Reports any task whose stack comes close to running out (the core's
// overflow hook only logs in debug builds and then carries on).
void platform_stack_check(void) {
    static uint32_t next;
    static TaskStatus_t st[12];
    static uint16_t low[12];   // lowest free words reported, per slot
    supply_task();
    if ((int32_t)(millis() - next) < 0) return;
    next = millis() + 2000;
    UBaseType_t n = uxTaskGetSystemState(st, 12, NULL);
    for (UBaseType_t i = 0; i < n; i++) {
        // The idle task's 400-byte stack is fixed by the core and its use is
        // steady (it runs no code of ours): not worth a warning.
        if (st[i].xHandle == xTaskGetIdleTaskHandle()) continue;
        uint16_t free_words = (uint16_t)st[i].usStackHighWaterMark;
        uint32_t id = st[i].xTaskNumber % 12;
        if (free_words < 64 && (low[id] == 0 || free_words < low[id])) {
            low[id] = free_words;
            LOG("stack: task %s has only %u bytes of stack left", st[i].pcTaskName, (unsigned)free_words * 4);
        }
    }
}

// GPREGRET2 keeps its value through every reset except losing power (it is
// cleared only by power-on / brown-out), and nothing else uses it (the
// bootloader uses GPREGRET). Set while the firmware runs, it tells a real
// power loss apart from a restart that leaves no reset reason (the firmware
// jumping back to its start, e.g. after memory corruption).

// Set the marker; through the SoftDevice once it runs (POWER is restricted then).
extern "C" void platform_set_restart_mark(uint8_t mark) {
    uint8_t sd = 0;
    sd_softdevice_is_enabled(&sd);
    if (sd) {
        sd_power_gpregret_clr(1, 0xFF);
        sd_power_gpregret_set(1, mark);
    } else {
        NRF_POWER->GPREGRET2 = mark;
    }
}

void platform_log_reset_reason(void) {
    // Called before the SoftDevice starts: the POWER registers are still ours.
    uint32_t r = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = r;   // write-1-to-clear, so the next boot sees fresh bits
    // RESETREAS is often already cleared here (this board's bootloader seems
    // to do it), so the retained marker decides: 0 = power was lost.
    uint8_t mark = (uint8_t)(NRF_POWER->GPREGRET2 & 0xFF);
    NRF_POWER->GPREGRET2 = ALIVE_MARK;
    const char *why = r & POWER_RESETREAS_DOG_Msk      ? "watchdog (main loop stuck)"
                    : r & POWER_RESETREAS_LOCKUP_Msk   ? "CPU lockup"
                    : mark == PLANNED_MARK             ? "restart by the firmware (update, mode change or reboot)"
                    : r & POWER_RESETREAS_SREQ_Msk     ? "software (crash)"
                    : r & POWER_RESETREAS_RESETPIN_Msk ? "reset button"
                    : r & POWER_RESETREAS_VBUS_Msk     ? "USB power"
                    : r ? "other"
                    : mark == ALIVE_MARK ? "unexpected restart (crash, hang or reset button; no reason recorded)"
                                         : "power on (power was off or dropped)";
    LOG("boot: last reset: %s (RESETREAS 0x%08lx, retained 0x%02x)", why, (unsigned long)r, mark);
    LOG("boot: supply %lu mV, USB %lu mV", (unsigned long)(analogReadVDD() * 3600UL / 1023),
        (unsigned long)(analogReadVDDHDIV5() * 3600UL * 5 / 1023));
    // How the chip is powered: from VDDH (USB 5 V through the on-chip REG0,
    // rated ~25 mA) or VDD directly; and whether the DC/DC converters are on
    // (without them the radio draws about twice the current at +8 dBm).
    uint32_t regout0 = NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk;
    static const char *const VOUT[] = {"1.8", "2.1", "2.4", "2.7", "3.0", "3.3"};
    LOG("boot: power %s, REG0 out %s V, DC/DC REG1 %s, REG0 %s",
        NRF_POWER->MAINREGSTATUS ? "high-voltage mode (USB 5 V -> on-chip REG0)" : "normal mode (3.3 V supplied externally)",
        regout0 < 6 ? VOUT[regout0] : "default 1.8", NRF_POWER->DCDCEN ? "on" : "off", NRF_POWER->DCDCEN0 ? "on" : "off");
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
