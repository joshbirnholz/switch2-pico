// Board-independent application glue: connects the Switch 2 link, the input
// mapping, the emulated Pro Controller and the configuration channels, and
// handles USB suspend / remote wakeup / re-enumeration. Each board's main
// calls app_core_init() once and app_core_task() from its main loop.

#include "app_core.h"

#include <string.h>

#include "tusb.h"

#include "app.h"
#include "ds5.h"
#include "log.h"
#include "mapping.h"
#include "platform.h"
#include "procon.h"
#include "s2_link.h"
#include "settings.h"
#include "usb_hid.h"
#include "usb_mode.h"
#include "webusb.h"

#define SUSPEND_DISCONNECT_MS 30000
#define COMBO_HOLD_MS         3000

static uint32_t s_raw_buttons;
static bool s_usb_reconnect;
static uint32_t s_usb_reconnect_at;
static int s_reboot;   // 0 none, 1 normal, 2 bootloader
static uint32_t s_reboot_at;

uint32_t app_raw_buttons(void) {
    return s_raw_buttons;
}

void app_request_usb_reconnect(void) {
    if (!s_usb_reconnect) {
        s_usb_reconnect = true;
        tud_disconnect();
        s_usb_reconnect_at = platform_deadline_ms(300);
        LOG("usb: re-enumerating");
    }
}

void app_request_reboot(bool bootloader) {
    s_reboot = bootloader ? 2 : 1;
    s_reboot_at = platform_deadline_ms(500);   // let the response go out first
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

void s2_link_hook_controller_seen(void) {
    static uint32_t next;
    if (!g_settings.usb_remote_wakeup || !tud_suspended() || !platform_time_reached(next)) return;
    next = platform_deadline_ms(1000);
    // Only works if the host armed remote wakeup (on Linux see
    // tools/99-switch2-pico.rules); TinyUSB ignores the request otherwise.
    LOG("usb: controller active, requesting host remote wakeup");
    tud_remote_wakeup();
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
    static mapping_macro_t macro;
    s2_input_t in;
    uint32_t seq;
    if (s2_link_get_input(&in, &seq)) {
        // A running macro advances on time, not only on new reports.
        if (was_connected && seq == last_seq && !mapping_macro_busy(&macro)) return;
        was_connected = true;
        last_seq = seq;
        uint32_t prev = s_raw_buttons;
        s_raw_buttons = in.buttons;
        // A button press while the host sleeps (controller still connected,
        // i.e. within SUSPEND_DISCONNECT_MS) wakes it.
        if ((in.buttons & ~prev) && tud_suspended()) s2_link_hook_controller_seen();
        if (usb_mode_active() == USB_MODE_DUALSENSE_EDGE) {
            // GL/GR/C are real buttons in this mode: no quick remap or macros.
            ds5_set_input(&in, s2_link_mapping_ctx(), true);
            return;
        }
        procon_input_t out;
        mapping_apply(&g_settings, s2_link_mapping_ctx(), &in, &out);
        if (!g_settings.quick_remap_off) {
            in_button_t back;
            if (mapping_quick_remap(&g_settings, prev, in.buttons, &back)) {
                uint8_t o = g_settings.button_map[back];
                LOG("remap: %s -> %s", in_button_name(back), o ? out_button_name((out_button_t)o) : "nothing");
                settings_save_later();
                s2_link_test_rumble();   // feedback on the controller
            }
            // Keep the chord's buttons away from the host.
            if (mapping_quick_remap_held(in.buttons)) out.buttons = 0;
        }
        out.buttons |= mapping_macro_step(&macro, &g_settings, prev, in.buttons, platform_millis());
        // Charge state byte is non-zero while external power is connected.
        procon_set_input(&out, true, in.battery_mv, in.charge_state != 0 && in.charge_state != 0x20);
    } else {
        if (!was_connected) return;
        was_connected = false;
        memset(&macro, 0, sizeof macro);
        s_raw_buttons = 0;
        procon_set_input(NULL, false, 0, false);
        ds5_set_input(NULL, NULL, false);
    }
}

bool app_combo_held(uint32_t combo, uint32_t hold_ms, bool *armed, uint32_t *since) {
    bool down = (s_raw_buttons & combo) == combo;
    if (!down) {
        *since = 0;
        *armed = true;
        return false;
    }
    if (!*since) *since = platform_millis() | 1u;
    if (*armed && platform_millis() - *since > hold_ms) {
        *armed = false;
        return true;
    }
    return false;
}

// Disconnect the controller (so it can sleep) while the USB host sleeps;
// a button press on the controller then wakes the host.
static void suspend_task(void) {
    static bool suspended;
    static uint32_t since;
    bool s = tud_suspended();
    if (s && !suspended) since = platform_millis();
    if (!s && suspended) s2_link_set_paused(false);
    suspended = s;
    if (s && platform_millis() - since > SUSPEND_DISCONNECT_MS) s2_link_set_paused(true);
}

static void maintenance_task(void) {
    if (s_usb_reconnect && platform_time_reached(s_usb_reconnect_at)) {
        s_usb_reconnect = false;
        if (!g_settings.usb_detach_when_idle || s2_link_state() == S2_LINK_READY) tud_connect();
    }
    if (s_reboot && platform_time_reached(s_reboot_at)) {
        platform_reboot(s_reboot == 2);
    }
}

void app_core_init(void) {
    LOG("usb: mode %s", usb_mode_name(usb_mode_active()));
    procon_init();
    ds5_init();
    s2_link_init();
}

void app_core_task(void) {
    platform_watchdog_feed();
    s2_link_task();
    update_input();
    usb_hid_task();
    if (usb_mode_active() == USB_MODE_DUALSENSE_EDGE) ds5_task();
    else procon_task();
    webusb_task();
    suspend_task();
    settings_task();
    maintenance_task();
}
