// Board-independent application glue: connects the Switch 2 link, the input
// mapping, the emulated Pro Controller and the configuration channels, and
// handles USB suspend / remote wakeup / re-enumeration. Each board's main
// calls app_core_init() once and app_core_task() from its main loop.

#include "app_core.h"

#include "tusb.h"

#include "app.h"
#include "log.h"
#include "mapping.h"
#include "mcu_nfc.h"
#include "platform.h"
#include "procon.h"
#include "s2_link.h"
#include "settings.h"
#include "usb_hid.h"
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
    procon_init();
    s2_link_init();
}

void app_core_task(void) {
    s2_link_task();
    update_input();
    usb_hid_task();
    procon_task();
    webusb_task();
    suspend_task();
    settings_task();
    maintenance_task();
}
