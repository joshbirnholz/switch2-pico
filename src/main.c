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
#include "bootsel.h"
#include "log.h"
#include "mapping.h"
#include "mcu_nfc.h"
#include "procon.h"
#include "s2_link.h"
#include "settings.h"
#include "status_led.h"
#include "web/wifi_ap.h"
#include "webusb.h"

#define HOTKEY_HOLD_MS        3000
#define BOOTSEL_FORGET_MS     5000
#define SUSPEND_DISCONNECT_MS 30000

static uint32_t s_raw_buttons;
static bool s_usb_reconnect;
static absolute_time_t s_usb_reconnect_at;
static int s_reboot;   // 0 none, 1 normal, 2 bootloader
static absolute_time_t s_reboot_at;

uint32_t app_raw_buttons(void) {
    return s_raw_buttons;
}

void app_request_usb_reconnect(void) {
    if (!s_usb_reconnect) {
        s_usb_reconnect = true;
        tud_disconnect();
        s_usb_reconnect_at = make_timeout_time_ms(300);
        LOG("usb: re-enumerating");
    }
}

void app_request_reboot(bool bootloader) {
    s_reboot = bootloader ? 2 : 1;
    s_reboot_at = make_timeout_time_ms(500);   // let the HTTP response go out
}

// ---------------------------------------------------------------------------
// Glue between the modules
// ---------------------------------------------------------------------------
void procon_hook_rumble(const rumble_sample_t *left, int nl, const rumble_sample_t *right, int nr) {
    s2_link_rumble_submit(left, nl, right, nr);
}

void procon_hook_player_lights(uint8_t lights) {
    if (!g_settings.led_follow_host) return;
    uint8_t pattern = lights & 0x0F;
    if (!pattern) pattern = (lights >> 4) & 0x0F;   // flashing lights -> show steady
    if (pattern) s2_link_set_player_leds(pattern);
}

void mcu_hook_polling_changed(bool polling) {
    s2_link_nfc_request(NFC_SRC_HOST, polling);
}

void mcu_hook_tag_written(void) {
    LOG("nfc: host wrote to the tag (cached copy updated; the physical tag is not modified)");
}

void s2_link_hook_controller_seen(void) {
    if (g_settings.usb_remote_wakeup && tud_suspended()) {
        LOG("usb: controller woke up, requesting host remote wakeup");
        tud_remote_wakeup();
    }
}

void s2_link_hook_connection_changed(bool connected) {
    if (g_settings.usb_detach_when_idle) {
        if (connected) tud_connect();
        else tud_disconnect();
    }
}

void s2_link_hook_controller_colors(const uint8_t rgb[12]) {
    // All-0xFF / all-zero means "not programmed"; keep our defaults then.
    bool valid = false;
    for (int i = 0; i < 12; i++) {
        if (rgb[i] != 0xFF && rgb[i] != 0x00) valid = true;
    }
    if (valid) procon_set_colors(rgb);
}

// ---------------------------------------------------------------------------
// Input path: latest Switch 2 report -> Pro Controller report fields
// ---------------------------------------------------------------------------
static void update_input(void) {
    static uint32_t last_seq;
    static bool was_connected;
    s2_input_t in;
    uint32_t seq;
    if (s2_link_get_input(&in, &seq)) {
        if (was_connected && seq == last_seq) return;
        was_connected = true;
        last_seq = seq;
        procon_input_t out;
        mapping_apply(&g_settings, s2_link_mapping_ctx(), &in, &out);
        s_raw_buttons = in.buttons;
        // Charge state byte is non-zero while external power is connected.
        procon_set_input(&out, true, in.battery_mv, in.charge_state != 0 && in.charge_state != 0x20);
    } else {
        if (!was_connected) return;
        was_connected = false;
        s_raw_buttons = 0;
        procon_set_input(NULL, false, 0, false);
    }
}

// C + Home held for 3 s toggles the configuration access point.
static void hotkey_task(void) {
    static bool armed = true;
    static absolute_time_t since;
    static bool held;
    const uint32_t combo = S2_BTN_C | S2_BTN_HOME;
    bool down = g_settings.hotkey_enabled && (s_raw_buttons & combo) == combo;
    if (down && !held) {
        held = true;
        since = get_absolute_time();
    } else if (!down) {
        held = false;
        armed = true;
    }
    if (held && armed && absolute_time_diff_us(since, get_absolute_time()) > HOTKEY_HOLD_MS * 1000) {
        armed = false;
        LOG("hotkey: toggling configuration access point");
        wifi_ap_toggle();
        s2_link_test_rumble();
    }
}

// BOOTSEL: short press toggles the access point, a 5 s hold forgets the paired controller.
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
        LOG("bootsel: forgetting paired controller");
        s2_link_forget();
    }
    if (!now && down && !long_done) {
        wifi_ap_toggle();
    }
    down = now;
}

// Disconnect the controller (so it can sleep) while the USB host sleeps;
// a button press on the controller then wakes the host.
static void suspend_task(void) {
    static bool suspended;
    static absolute_time_t since;
    bool s = tud_suspended();
    if (s && !suspended) since = get_absolute_time();
    if (!s && suspended) s2_link_set_paused(false);
    suspended = s;
    if (s && absolute_time_diff_us(since, get_absolute_time()) > SUSPEND_DISCONNECT_MS * 1000) {
        s2_link_set_paused(true);
    }
}

static void maintenance_task(void) {
    if (s_usb_reconnect && time_reached(s_usb_reconnect_at)) {
        s_usb_reconnect = false;
        if (!g_settings.usb_detach_when_idle || s2_link_state() == S2_LINK_READY) tud_connect();
    }
    if (s_reboot && time_reached(s_reboot_at)) {
        if (s_reboot == 2) {
            reset_usb_boot(0, 0);
        } else {
            watchdog_reboot(0, 0, 10);
        }
        for (;;) tight_loop_contents();
    }
}

int main(void) {
    board_init();
    stdio_init_all();
    log_init();
    LOG("switch2-pico %s starting", S2P_VERSION);
    settings_init();

    procon_init();
    tud_init(0);
    if (g_settings.usb_detach_when_idle) tud_disconnect();

    if (cyw43_arch_init()) {
        LOG("fatal: CYW43 init failed");
        for (;;) tud_task();
    }
    s2_link_init();
    wifi_ap_init();
    if (g_settings.wifi_autostart && !g_settings.bonded) wifi_ap_start();

    for (;;) {
        tud_task();
        cyw43_arch_poll();
        s2_link_task();
        update_input();
        procon_task();
        hotkey_task();
        bootsel_task();
        suspend_task();
        wifi_ap_task();
        webusb_task();
        status_led_task();
        settings_task();
        maintenance_task();
    }
}
