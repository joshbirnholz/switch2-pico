// Board-independent application glue: connects the Switch 2 link, the input
// mapping, the emulated Pro Controller and the configuration channels, and
// handles USB suspend / remote wakeup / re-enumeration. Each board's main
// calls app_core_init() once and app_core_task() from its main loop.

#include "app_core.h"

#include <string.h>

#include "tusb.h"

#include "app.h"
#include "battery.h"
#include "ds5.h"
#include "x360.h"
#include "gc_adapter.h"
#include "log.h"
#include "mapping.h"
#include "mode_select.h"
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
    // Save the log around a lost connection right away (it may be followed
    // by a reset or power loss).
    if (!connected) platform_log_flush();
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
// USB mode selection on the controller (mode_select.h): the controller's LEDs
// blink while it is active, the host sees no buttons, and buttons still held
// when it ends stay hidden from the host until released.
// ---------------------------------------------------------------------------
#define MODE_BLINK_MS      170
#define MODE_SWITCH_DELAY  250   // let the "chosen" thump play before the flash write

static mode_select_t s_msel;
static uint32_t s_swallow;            // raw buttons kept from the host until released
static uint32_t s_blink_at;
static bool s_blink_on;
static int s_mode_switch = -1;        // usb_mode_t to switch to, -1: none
static uint32_t s_mode_switch_at;

void app_mode_select_cancel(void) {
    if (!s_msel.active) return;
    mode_select_cancel(&s_msel);
    s_swallow = s_raw_buttons;
    s2_link_set_led_override(-1);
    LOG("mode select: cancelled");
}

static void mode_select_step(uint32_t raw) {
    usb_mode_t chosen;
    uint32_t now = platform_millis();
    switch (mode_select_update(&s_msel, g_settings.mode_slot, raw, now, &chosen)) {
    case MODE_SELECT_ENTER:
        LOG("mode select: press a button with a mode (C + Home again to leave)");
        s2_link_haptic(S2_HAPTIC_BA_THUMP);
        s_blink_on = true;
        s_blink_at = now;
        s2_link_set_led_override(0x0F);
        break;
    case MODE_SELECT_CANCEL:
        LOG("mode select: left without a change");
        s_swallow = raw;
        s2_link_set_led_override(-1);
        break;
    case MODE_SELECT_CHOSEN:
        s_swallow = raw;
        if (chosen == usb_mode_active() && chosen == g_settings.usb_mode) {
            LOG("mode select: already %s", usb_mode_name(chosen));
            s2_link_haptic(S2_HAPTIC_TICK);
            s2_link_set_led_override(-1);
        } else {
            LOG("mode select: %s, restarting", usb_mode_name(chosen));
            s2_link_haptic(S2_HAPTIC_THUMP);
            s2_link_set_led_override(0x0F);
            s_mode_switch = chosen;
            s_mode_switch_at = platform_deadline_ms(MODE_SWITCH_DELAY);
        }
        break;
    default:
        break;
    }
    if (s_msel.active && now - s_blink_at >= MODE_BLINK_MS) {
        s_blink_at = now;
        s_blink_on = !s_blink_on;
        s2_link_set_led_override(s_blink_on ? 0x0F : 0x00);
    }
}

static void mode_select_task(void) {
    // A chosen mode is applied even if the controller drops meanwhile.
    if (s_mode_switch >= 0 && platform_time_reached(s_mode_switch_at)) {
        g_settings.usb_mode = (uint8_t)s_mode_switch;
        s_mode_switch = -1;
        settings_save_now();
        app_request_reboot(false);
        return;
    }
    if (s2_link_state() != S2_LINK_READY) {
        if (s_msel.active) {
            mode_select_cancel(&s_msel);
            s2_link_set_led_override(-1);
        }
        return;
    }
    mode_select_step(s_raw_buttons);
}

// The buttons the host may see, from the raw ones.
static uint32_t host_buttons(uint32_t raw) {
    s_swallow &= raw;
    if (s_msel.active || s_mode_switch >= 0) return 0;
    raw &= ~s_swallow;
    // C + Home is a shortcut (mode selection; on Wi-Fi boards also the access
    // point): keep Home from the host while C is held, so holding it doesn't
    // open the host's home menu first.
    bool shortcut = mode_select_enabled(g_settings.mode_slot) || (g_settings.hotkey_enabled && platform_has_wifi());
    if (shortcut && (raw & S2_BTN_C)) raw &= ~S2_BTN_HOME;
    return raw;
}

// ---------------------------------------------------------------------------
// Inactivity: disconnect an unused controller so it sleeps (saves its
// battery). Activity = a button change or a stick moving more than
// IDLE_STICK_DELTA (raw 12-bit) from where it was at the last activity.
// ---------------------------------------------------------------------------
#define IDLE_STICK_DELTA 250

static uint32_t s_idle_since;
static uint16_t s_idle_sticks[4];

static void idle_reset(const s2_input_t *in) {
    s_idle_since = platform_millis();
    s_idle_sticks[0] = in->stick_l[0];
    s_idle_sticks[1] = in->stick_l[1];
    s_idle_sticks[2] = in->stick_r[0];
    s_idle_sticks[3] = in->stick_r[1];
}

static void idle_check(const s2_input_t *in, uint32_t prev_buttons, bool first) {
    const uint16_t now[4] = {in->stick_l[0], in->stick_l[1], in->stick_r[0], in->stick_r[1]};
    bool active = first || in->buttons != prev_buttons;
    for (int i = 0; i < 4 && !active; i++) {
        int d = (int)now[i] - (int)s_idle_sticks[i];
        if (d > IDLE_STICK_DELTA || d < -IDLE_STICK_DELTA) active = true;
    }
    if (active) {
        idle_reset(in);
        return;
    }
    if (g_settings.idle_disconnect_off || s_msel.active || s_mode_switch >= 0) return;
    uint32_t limit = (uint32_t)g_settings.idle_minutes * 60000u;
    if (platform_millis() - s_idle_since >= limit) {
        LOG("idle: no input for %u min, disconnecting the controller so it can sleep", g_settings.idle_minutes);
        idle_reset(in);
        s2_link_let_controller_sleep();
    }
}

// Sticks centered and analog triggers released (buttons are left as they are).
static void neutral_input(s2_input_t *in) {
    const mapping_ctx_t *c = s2_link_mapping_ctx();
    memcpy(in->stick_l, c->cal_l.center, sizeof in->stick_l);
    memcpy(in->stick_r, c->cal_r.center, sizeof in->stick_r);
    in->trigger_l = c->gc_trigger_neutral[0];
    in->trigger_r = c->gc_trigger_neutral[1];
}

// ---------------------------------------------------------------------------
// Input path: latest Switch 2 report -> Pro Controller report fields
// ---------------------------------------------------------------------------
static void update_input(void) {
    static uint32_t last_seq;
    static bool was_connected;
    static mapping_macro_t macro;
    static uint32_t host_prev;
    s2_input_t in;
    uint32_t seq;
    if (s2_link_get_input(&in, &seq)) {
        // A running macro advances on time, not only on new reports.
        if (was_connected && seq == last_seq && !mapping_macro_busy(&macro)) return;
        bool first = !was_connected;   // newly connected: starts the idle timer
        was_connected = true;
        last_seq = seq;
        uint32_t raw_prev = s_raw_buttons;
        idle_check(&in, raw_prev, first);
        // Charge state byte is non-zero while external power is connected.
        battery_update(&g_battery, in.battery_mv, in.charge_state != 0 && in.charge_state != 0x20, platform_millis());
        s_raw_buttons = in.buttons;
        // A button press while the host sleeps (controller still connected,
        // i.e. within SUSPEND_DISCONNECT_MS) wakes it.
        if ((in.buttons & ~raw_prev) && tud_suspended()) s2_link_hook_controller_seen();
        mode_select_step(in.buttons);
        // From here on, `in` holds what the host may see.
        uint32_t prev = host_prev;
        in.buttons = host_prev = host_buttons(in.buttons);
        // Selecting a mode: sticks centered, triggers released.
        if (s_msel.active || s_mode_switch >= 0) neutral_input(&in);
        usb_mode_t mode = usb_mode_active();
        if (mode != USB_MODE_SWITCH_PRO) {
            // Generic modes: the mode's own map (gp_out_t), same shortcuts.
            uint8_t *map = g_settings.mode_map[mode];
            const mapping_ctx_t *ctx = s2_link_mapping_ctx();
            // Not in DualSense Edge mode: GL/GR/C are its paddles and Fn
            // buttons, which the host's software remaps itself.
            bool quick = !g_settings.quick_remap_off && mode != USB_MODE_DUALSENSE_EDGE;
            if (quick) {
                in_button_t back;
                if (mapping_quick_remap_map(map, prev, in.buttons, &back)) {
                    const char *n = usb_mode_output_name(mode, (gp_out_t)map[back]);
                    LOG("remap: %s -> %s", in_button_name(back), n ? n : "?");
                    settings_save_later();
                    s2_link_haptic(S2_HAPTIC_TICK);
                }
            }
            uint32_t gp;
            switch (mapping_macro_run(&macro, map, GP_MACRO_QAM, prev, in.buttons, platform_millis())) {
            // While the quick access shortcut plays, the host sees only its
            // buttons: nothing else pressed, sticks centered.
            case MACRO_GUIDE:
                gp = GP_BIT(GP_GUIDE);
                neutral_input(&in);
                break;
            case MACRO_GUIDE_SOUTH:
                gp = GP_BIT(GP_GUIDE) | GP_BIT(GP_SOUTH);
                neutral_input(&in);
                break;
            default:
                gp = mapping_gp_buttons(&g_settings, map, ctx, &in);
                if (quick && mapping_quick_remap_held(in.buttons)) gp = 0;
                break;
            }
            if (mode == USB_MODE_XBOX360) x360_set_input(&in, ctx, gp, true);
            else if (mode == USB_MODE_GC_ADAPTER) gc_adapter_set_input(&in, ctx, gp, true);
            else ds5_set_input(&in, ctx, gp, true);
            return;
        }
        procon_input_t out;
        // Home+A shortcut playing: only its buttons, sticks centered.
        uint32_t macro_buttons = mapping_macro_step(&macro, &g_settings, prev, in.buttons, platform_millis());
        if (macro_buttons) neutral_input(&in);
        mapping_apply(&g_settings, s2_link_mapping_ctx(), &in, &out);
        if (macro_buttons) {
            out.buttons = macro_buttons;
        } else if (!g_settings.quick_remap_off) {
            in_button_t back;
            if (mapping_quick_remap(&g_settings, prev, in.buttons, &back)) {
                uint8_t o = g_settings.button_map[back];
                LOG("remap: %s -> %s", in_button_name(back), o ? out_button_name((out_button_t)o) : "nothing");
                settings_save_later();
                s2_link_haptic(S2_HAPTIC_TICK);   // feedback on the controller
            }
            // Keep the chord's buttons away from the host.
            if (mapping_quick_remap_held(in.buttons)) out.buttons = 0;
        }
        // Charge state byte is non-zero while external power is connected.
        procon_set_input(&out, true, in.battery_mv, in.charge_state != 0 && in.charge_state != 0x20);
    } else {
        if (!was_connected) return;
        was_connected = false;
        memset(&macro, 0, sizeof macro);
        battery_reset(&g_battery);
        s_raw_buttons = 0;
        host_prev = 0;
        s_swallow = 0;
        procon_set_input(NULL, false, 0, false);
        ds5_set_input(NULL, NULL, 0, false);
        x360_set_input(NULL, NULL, 0, false);
        gc_adapter_set_input(NULL, NULL, 0, false);
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
    // Signed: `since` may be 1 ms ahead of now (it is never 0).
    if (*armed && (int32_t)(platform_millis() - *since) > (int32_t)hold_ms) {
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

// Why the host connection went away: USB power gone (port, cable or hub) or
// the host dropped / reset the device with power still there.
static void usb_watch_task(void) {
    static bool was_mounted;
    bool m = tud_mounted();
    if (m == was_mounted) return;
    was_mounted = m;
    if (m) return;
    platform_supply_t sup;
    bool have_sup = platform_supply(&sup);
    int p = platform_usb_power();
    LOG("usb: host connection lost (%s%s%u mV)", p == 1 ? "USB power still present: the host reset or dropped the device"
                                                : p == 0 ? "USB power lost: port, cable or hub"
                                                         : "USB power unknown",
        have_sup ? ", USB supply " : "", have_sup ? sup.vbus : 0);
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
    mode_select_init(&s_msel);
    battery_reset(&g_battery);
    procon_init();
    ds5_init();
    x360_init();
    gc_adapter_init();
    s2_link_init();
}

void app_core_task(void) {
    platform_watchdog_feed();
    s2_link_task();
    update_input();
    mode_select_task();
    usb_hid_task();
    switch (usb_mode_active()) {
    case USB_MODE_DUALSENSE_EDGE:
    case USB_MODE_DUALSENSE: ds5_task(); break;
    case USB_MODE_XBOX360: x360_task(); break;
    case USB_MODE_GC_ADAPTER: gc_adapter_task(); break;
    default: procon_task(); break;
    }
    webusb_task();
    suspend_task();
    usb_watch_task();
    settings_task();
    maintenance_task();
}
