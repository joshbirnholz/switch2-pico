// Switch2-Pico: Nintendo Switch 2 controller -> USB Switch Pro Controller bridge
// for the Raspberry Pi Pico 2 W.
//
// Everything runs from one cooperative loop on core 0:
//   * TinyUSB device stack (emulated Pro Controller)
//   * CYW43 driver in poll mode, which drives BTstack (Bluetooth LE central)
//     and lwIP (the on-demand configuration access point)

#include <stdio.h>

#include "bsp/board_api.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "app.h"
#include "app_core.h"
#include "platform.h"
#include "version.h"
#include "bootsel.h"
#include "log.h"
#include "mapping.h"
#include "procon.h"
#include "s2_link.h"
#include "settings.h"
#include "usb_mode.h"
#include "status_led.h"
#include "usb_hid.h"
#include "web/wifi_ap.h"
#include "webusb.h"

#define HOTKEY_HOLD_MS        3000
#define BOOTSEL_FORGET_MS     5000

void app_wifi_stop(void) {
    wifi_ap_stop_later();
}

void platform_reboot(bool bootloader) {
    if (bootloader) {
        reset_usb_boot(0, 0);
    } else {
        watchdog_reboot(0, 0, 10);
    }
    for (;;) tight_loop_contents();
}

// C + Home held for 3 s toggles the configuration access point (after 1.5 s
// it has also entered USB mode selection, which this leaves again).
static void hotkey_task(void) {
    static bool armed = true;
    static uint32_t since;
    if (!g_settings.hotkey_enabled) return;
    if (app_combo_held(S2_BTN_C | S2_BTN_HOME, HOTKEY_HOLD_MS, &armed, &since)) {
        LOG("hotkey: toggling configuration access point");
        wifi_ap_toggle();
        app_mode_select_cancel();
        s2_link_haptic(S2_HAPTIC_TICK);
    }
}

// BOOTSEL: a 5 s hold forgets the paired controllers. A short press toggles
// the access point, or, with "pair only after Sync" on, acts as the Sync
// button (opens / closes the pairing window); the access point is then a
// 1-5 s hold.
#define PAIR_WINDOW_MS   60000
#define BOOTSEL_SHORT_MS 1000

static void bootsel_task(void) {
    static absolute_time_t next_sample;
    static bool down;
    static absolute_time_t since;
    static bool long_done;
    if (!time_reached(next_sample)) return;
    next_sample = make_timeout_time_ms(50);
    bool now = bootsel_pressed();
    if (now && !down) {
        since = get_absolute_time();
        long_done = false;
    }
    if (now && !long_done && absolute_time_diff_us(since, get_absolute_time()) > BOOTSEL_FORGET_MS * 1000) {
        long_done = true;
        LOG("bootsel: forgetting paired controllers");
        s2_link_forget(NULL);
    }
    if (!now && down && !long_done) {
        bool short_press = absolute_time_diff_us(since, get_absolute_time()) < BOOTSEL_SHORT_MS * 1000;
        if (g_settings.pair_button && short_press) {
            if (s2_link_pairing_open()) s2_link_stop_pairing();
            else s2_link_start_pairing(PAIR_WINDOW_MS);
        } else {
            wifi_ap_toggle();
        }
    }
    down = now;
}

int main(void) {
    // A firmware update staged from the configuration page installs here,
    // before anything else runs (the board restarts when done).
    platform_fw_apply_if_pending();
    board_init();
    stdio_init_all();
    log_init();
    LOG("switch2-pico %s starting", S2P_VERSION);
    // Only a real timeout counts; platform_reboot() also resets via the watchdog.
    if (watchdog_enable_caused_reboot()) LOG("boot: last reset: watchdog (main loop stuck)");
    settings_init();

    // No controller yet: start as the configuration-only device.
    usb_mode_set_config_only(platform_usb_config_only_supported());
    tud_init(0);

    if (cyw43_arch_init()) {
        LOG("fatal: CYW43 init failed");
        for (;;) tud_task();
    }
    app_core_init();
    wifi_ap_init();
    if (g_settings.wifi_autostart && !g_settings.bonded) wifi_ap_start();

    // Reset if the main loop stops for 8 s (fed from app_core_task()).
    watchdog_enable(8000, true);

    for (;;) {
        tud_task();
        cyw43_arch_poll();
        app_core_task();
        hotkey_task();
        bootsel_task();
        wifi_ap_task();
        status_led_task();
    }
}
