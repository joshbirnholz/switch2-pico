// Board-independent application glue: connects the Switch 2 link, the input
// mapping, the emulated Pro Controller and the configuration channels, and
// handles USB suspend / remote wakeup / re-enumeration. Each board's main
// calls app_core_init() once and app_core_task() from its main loop.

#include "app_core.h"

#include <math.h>
#include <string.h>

#include "tusb.h"

#include "app.h"
#include "battery.h"
#include "ds5.h"
#include "x360.h"
#include "gc_adapter.h"
#include "sinput.h"
#include "joycon.h"
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

static uint32_t s_decky_qam;

uint32_t app_decky_qam_presses(void) {
    return s_decky_qam;
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

// The dongle appears as a controller only while one is connected; otherwise
// it is a configuration-only USB device (see usb_mode_set_config_only()).
// Switching means leaving the bus and coming back as the other device, so:
// - a controller connecting switches right away;
// - one going away switches only after USB_GONE_MS (a dropped connection
//   that comes right back doesn't make the host see two new devices);
// - not while the host sleeps (the controller is let go then, and a button
//   press has to wake the host through the device it armed), nor for
//   USB_RESUMED_MS after it wakes (the controller reconnects meanwhile).
#define USB_GONE_MS    5000
#define USB_RESUMED_MS 15000

static void usb_present_controller(bool controller) {
    static bool waiting;
    static uint32_t switch_at;
    bool config_only = !controller && platform_usb_config_only_supported();
    if (config_only && !usb_mode_config_only()) {
        if (tud_suspended()) {
            waiting = true;
            switch_at = platform_deadline_ms(USB_RESUMED_MS);
            return;
        }
        if (!waiting) {
            waiting = true;
            switch_at = platform_deadline_ms(USB_GONE_MS);
        }
        if (!platform_time_reached(switch_at)) return;
    }
    waiting = false;
    if (config_only == usb_mode_config_only()) return;
    tud_disconnect();
    usb_mode_set_config_only(config_only);
    platform_usb_rebuild();
    s_usb_reconnect = true;
    s_usb_reconnect_at = platform_deadline_ms(300);
    LOG("usb: %s", config_only ? "no controller: configuration-only device" : "controller connected: emulating it");
}

void s2_link_hook_connection_changed(bool connected) {
    // Save the log around a lost connection right away (it may be followed
    // by a reset or power loss).
    if (!connected) platform_log_flush();
    usb_present_controller(connected);
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
static int s_mode_switch = -1;        // profile to switch to (with a restart), -1: none
static ctrl_type_t s_mode_switch_type; // ... of this controller type
static uint32_t s_mode_switch_at;

void app_mode_select_cancel(void) {
    if (!s_msel.active) return;
    mode_select_cancel(&s_msel);
    s_swallow = s_raw_buttons;
    s2_link_set_led_override(-1);
    LOG("mode select: cancelled");
}

static void mode_select_step(uint32_t raw) {
    uint8_t chosen;
    uint32_t now = platform_millis();
    ctrl_type_t type = mapping_ctrl_type(s2_link_mapping_ctx());
    ctrl_profiles_t *profiles = settings_profiles(&g_settings, type);
    uint8_t slots[MODE_SLOT_COUNT];
    settings_profile_slots(&g_settings, type, slots);
    if (!mode_select_for(type)) {
        // No shortcut on a single Joy-Con 2 (chosen on the page instead).
        if (s_msel.active) {
            mode_select_cancel(&s_msel);
            s2_link_set_led_override(-1);
        }
        return;
    }
    switch (mode_select_update(&s_msel, slots, raw, now, &chosen)) {
    case MODE_SELECT_ENTER:
        LOG("profile select: press a button with a profile (C + Home again to leave)");
        s2_link_haptic(S2_HAPTIC_BA_THUMP);
        s_blink_on = true;
        s_blink_at = now;
        s2_link_set_led_override(0x0F);
        break;
    case MODE_SELECT_CANCEL:
        LOG("profile select: left without a change");
        s_swallow = raw;
        s2_link_set_led_override(-1);
        break;
    case MODE_SELECT_CHOSEN: {
        s_swallow = raw;
        const profile_t *p = &profiles->p[chosen];
        if (chosen == profiles->active) {
            LOG("profile select: already %s", p->name);
            s2_link_haptic(S2_HAPTIC_TICK);
            s2_link_set_led_override(-1);
        } else if (p->usb_mode == usb_mode_active() && settings_mouse_interfaces(p, type) == usb_mode_has_mouse()) {
            // Same USB device: only the map and options change, right away.
            LOG("profile select: %s", p->name);
            profiles->active = chosen;
            settings_save_later();
            s2_link_haptic(S2_HAPTIC_THUMP);
            s2_link_set_led_override(-1);
        } else {
            LOG("profile select: %s (%s), restarting", p->name, usb_mode_name((usb_mode_t)p->usb_mode));
            s2_link_haptic(S2_HAPTIC_THUMP);
            s2_link_set_led_override(0x0F);
            s_mode_switch = chosen;
            s_mode_switch_type = type;
            s_mode_switch_at = platform_deadline_ms(MODE_SWITCH_DELAY);
        }
        break;
    }
    default:
        break;
    }
    if (s_msel.active && now - s_blink_at >= MODE_BLINK_MS) {
        s_blink_at = now;
        s_blink_on = !s_blink_on;
        s2_link_set_led_override(s_blink_on ? 0x0F : 0x00);
    }
}

// A controller of type t is about to connect. Each type has its own active
// profile: when its USB mode (or whether it uses the USB mouse) differs from
// what the dongle runs as, remember t and restart into it (the controller
// keeps advertising meanwhile and connects after the restart). True while
// restarting: don't connect.
bool app_controller_type(ctrl_type_t t) {
    static bool restarting;
    if (restarting) return true;
    if (g_settings.last_ctrl != t) {
        g_settings.last_ctrl = (uint8_t)t;
        const profile_t *p = settings_active(&g_settings, t);
        uint8_t m = p->usb_mode;
        if (m != usb_mode_active() || settings_mouse_interfaces(p, t) != usb_mode_has_mouse()) {
            LOG("usb: %s: its profile is %s%s, restarting", ctrl_type_name(t), usb_mode_name((usb_mode_t)m),
                settings_mouse_interfaces(p, t) ? " with the mice" : "");
            restarting = true;
            settings_save_now();
            app_request_reboot(false);
            return true;
        }
        settings_save_later();
    }
    return false;
}

// Once a minute while connected: what the battery estimate is based on.
static void battery_log(void) {
    static uint32_t next;
    if (!platform_time_reached(next)) return;
    next = platform_deadline_ms(60000);
    const battery_t *b = &g_battery;
    if (b->has_level) {
        LOG("battery: controller level %u/9 -> %u%%%s (%u mV, state 0x%02x)", b->level, b->pct,
            b->full ? " full" : b->charging ? " charging" : "", b->last_mv, b->last_state);
        return;
    }
    LOG("battery: %u mV now (last window %u-%u), estimate %u mV, state 0x%02x, %u%%%s", b->last_mv, b->win_min,
        b->win_max, (unsigned)(b->mv + 0.5f), b->last_state, b->pct,
        b->full ? " full" : b->charging ? " charging" : "");
}

static void mode_select_task(void) {
    // A chosen profile is applied even if the controller drops meanwhile.
    if (s_mode_switch >= 0 && platform_time_reached(s_mode_switch_at)) {
        settings_profiles(&g_settings, s_mode_switch_type)->active = (uint8_t)s_mode_switch;
        g_settings.last_ctrl = (uint8_t)s_mode_switch_type;
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
    ctrl_type_t t = mapping_ctrl_type(s2_link_mapping_ctx());
    uint8_t slots[MODE_SLOT_COUNT];
    settings_profile_slots(&g_settings, t, slots);
    bool shortcut = (mode_select_for(t) && mode_select_enabled(slots)) || (g_settings.hotkey_enabled && platform_has_wifi());
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
// Mouse Mode (USB mice, see usb_hid.h): each Joy-Con 2 is its own mouse
// while it lies on a surface (the (L) the first USB mouse, the (R) the
// second), as on a Switch 2. Its optical sensor moves the pointer, its
// shoulder clicks, trigger right-clicks, stick click middle-clicks and its
// stick scrolls (up or left: up; down or right: down); those aren't the
// controller's while it is a mouse.
// ---------------------------------------------------------------------------
#define MOUSE_SCROLL_HZ      12.0f   // wheel steps per second at full deflection
#define MOUSE_SCROLL_DEAD    0.25f
// The sensor's lift-off distance (see s2_input_t): on a surface below the
// first, lifted above the second.
#define MOUSE_ON_SURFACE     800
#define MOUSE_LIFTED         1500

static const uint16_t MOUSE_PID[USB_MOUSE_COUNT] = {S2_PID_JOYCON2_L, S2_PID_JOYCON2_R};
static bool s_mouse_down[USB_MOUSE_COUNT];   // that Joy-Con is a mouse now

// Mouse Mode is on for what's connected (and the USB mice are there).
static bool mouse_mode(void) {
    ctrl_type_t t = mapping_ctrl_type(s2_link_mapping_ctx());
    return usb_mode_has_mouse() && settings_profile_mouse(settings_active(&g_settings, t), t);
}

// Which Joy-Con 2 lie on a surface (with some hysteresis).
static void mouse_pick(void) {
    bool on = mouse_mode();
    for (int i = 0; i < USB_MOUSE_COUNT; i++) {
        s2_input_t side;
        bool down = false;
        if (on && s2_link_mouse_ready(MOUSE_PID[i]) && s2_link_side_input(MOUSE_PID[i], &side, NULL)) {
            down = s_mouse_down[i] ? side.mouse_distance < MOUSE_LIFTED : side.mouse_distance < MOUSE_ON_SURFACE;
        }
        if (down != s_mouse_down[i]) {
            s_mouse_down[i] = down;
            LOG("mouse: Joy-Con 2 (%c) %s", i ? 'R' : 'L',
                down ? "on a surface: it's a mouse" : "lifted: its buttons are the controller's");
        }
    }
}

// What the mice take, the gamepad doesn't get.
static void mouse_take_from_gamepad(s2_input_t *in) {
    mouse_pick();
    const mapping_ctx_t *c = s2_link_mapping_ctx();
    for (int i = 0; i < USB_MOUSE_COUNT; i++) {
        if (!s_mouse_down[i]) continue;
        joycon_mouse_buttons_t mb;
        joycon_mouse_buttons(mapping_ctrl_type(c), i == 0, &mb);
        in->buttons &= ~(mb.left | mb.right | mb.middle);
        if (mb.stick_left) memcpy(in->stick_l, c->cal_l.center, sizeof in->stick_l);
        else memcpy(in->stick_r, c->cal_r.center, sizeof in->stick_r);
    }
}

// One Joy-Con's mouse: movement, buttons, wheel.
typedef struct {
    int32_t acc_x, acc_y;     // hundredths of a count, not sent yet
    float acc_wheel, acc_pan;
    uint8_t sent_buttons;
    bool collected;
} mouse_state_t;

static int32_t clamp127(int32_t v) {
    return v > 127 ? 127 : v < -127 ? -127 : v;
}

static void mouse_one(int i, mouse_state_t *m, float dt) {
    int32_t dx = 0, dy = 0;
    // Movement while it isn't a mouse (held, lifted) is dropped.
    bool linked = s2_link_mouse_take(MOUSE_PID[i], &dx, &dy);
    uint8_t buttons = 0;
    if (linked && s_mouse_down[i]) {
        ctrl_type_t t = mapping_ctrl_type(s2_link_mapping_ctx());
        int32_t ox, oy;
        joycon_mouse_apply(settings_active(&g_settings, t), dx, dy, &ox, &oy);
        m->acc_x += ox;
        m->acc_y += oy;
        joycon_mouse_buttons_t mb;
        joycon_mouse_buttons(t, i == 0, &mb);
        if (!s_msel.active) {
            if (s_raw_buttons & mb.left) buttons |= USB_MOUSE_LEFT;
            if (s_raw_buttons & mb.right) buttons |= USB_MOUSE_RIGHT;
            if (s_raw_buttons & mb.middle) buttons |= USB_MOUSE_MIDDLE;
        }
        // Scrolling (see joycon_mouse_scroll()).
        s2_input_t side;
        s2_stick_cal_t cal;
        if (s2_link_side_input(MOUSE_PID[i], &side, &cal)) {
            const uint16_t *raw = i == 0 ? side.stick_l : side.stick_r;
            float x = s2_stick_axis(&cal, 0, raw[0]), y = s2_stick_axis(&cal, 1, raw[1]);
            float wv, pv;
            joycon_mouse_scroll(i == 0, x, y, settings_active(&g_settings, t)->mouse_flags, &wv, &pv);
            if (fabsf(wv) > MOUSE_SCROLL_DEAD) m->acc_wheel += wv * MOUSE_SCROLL_HZ * dt;
            if (fabsf(pv) > MOUSE_SCROLL_DEAD) m->acc_pan += pv * MOUSE_SCROLL_HZ * dt;
        }
    } else {
        m->acc_x = m->acc_y = 0;
        m->acc_wheel = m->acc_pan = 0.0f;
    }
    // Not sent yet (endpoint busy, host asleep): never more than a few reports' worth.
    const int32_t lim = 4 * 127 * 100;
    if (m->acc_x > lim) m->acc_x = lim;
    if (m->acc_x < -lim) m->acc_x = -lim;
    if (m->acc_y > lim) m->acc_y = lim;
    if (m->acc_y < -lim) m->acc_y = -lim;
    if (m->acc_wheel > 127.0f) m->acc_wheel = 127.0f;
    if (m->acc_wheel < -127.0f) m->acc_wheel = -127.0f;
    if (m->acc_pan > 127.0f) m->acc_pan = 127.0f;
    if (m->acc_pan < -127.0f) m->acc_pan = -127.0f;
    int32_t x = clamp127(m->acc_x / 100), y = clamp127(m->acc_y / 100), w = clamp127((int32_t)m->acc_wheel);
    int32_t pan = clamp127((int32_t)m->acc_pan);
    if (!x && !y && !w && !pan && buttons == m->sent_buttons) return;
    if (!usb_mouse_send((uint8_t)i, buttons, (int8_t)x, (int8_t)y, (int8_t)w, (int8_t)pan)) return;
    if (!m->collected && s_mouse_down[i]) {
        m->collected = true;
        LOG("mouse: Joy-Con 2 (%c) sending to the host", i ? 'R' : 'L');
    }
    m->acc_x -= x * 100;
    m->acc_y -= y * 100;
    m->acc_wheel -= (float)w;
    m->acc_pan -= (float)pan;
    m->sent_buttons = buttons;
}

static void mouse_task(void) {
    static mouse_state_t mice[USB_MOUSE_COUNT];
    static uint32_t last_ms;
    uint32_t now = platform_millis();
    float dt = (float)(now - last_ms) / 1000.0f;
    last_ms = now;
    if (dt > 0.1f) dt = 0.1f;
    mouse_pick();
    for (int i = 0; i < USB_MOUSE_COUNT; i++) mouse_one(i, &mice[i], dt);
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
        battery_update(&g_battery, in.battery_mv, in.charge_state, platform_millis());
        battery_log();
        s_raw_buttons = in.buttons;
        // A button press while the host sleeps (controller still connected,
        // i.e. within SUSPEND_DISCONNECT_MS) wakes it.
        if ((in.buttons & ~raw_prev) && tud_suspended()) s2_link_hook_controller_seen();
        mode_select_step(in.buttons);
        // "Quick Access Menu (Decky)": counted on a tap of the controller's
        // own buttons (the host's view hides Home while C is held).
        {
            static mapping_tap_t qam_tap;
            usb_mode_t m = usb_mode_active();
            if (first) memset(&qam_tap, 0, sizeof qam_tap);
            if (mapping_tap(&qam_tap, settings_active_map(&g_settings, mapping_ctrl_type(s2_link_mapping_ctx())),
                            m == USB_MODE_SWITCH_PRO ? OUT_DECKY_QAM : GP_DECKY_QAM, raw_prev, in.buttons)) {
                s_decky_qam++;
            }
        }
        // From here on, `in` holds what the host may see.
        uint32_t prev = host_prev;
        in.buttons = host_prev = host_buttons(in.buttons);
        // Selecting a mode: sticks centered, triggers released.
        if (s_msel.active || s_mode_switch >= 0) neutral_input(&in);
        mouse_take_from_gamepad(&in);
        usb_mode_t mode = usb_mode_active();
        if (mode != USB_MODE_SWITCH_PRO) {
            // Generic modes: the mode's own map (gp_out_t), same shortcuts.
            const mapping_ctx_t *ctx = s2_link_mapping_ctx();
            // The active profile's map (its mode is the one we run in).
            uint8_t *map = settings_active_map(&g_settings, mapping_ctrl_type(ctx));
            // Not in DualSense Edge or SInput mode: GL/GR/C are their paddles
            // and extra buttons, which the host's software remaps itself.
            bool quick = !g_settings.quick_remap_off && mode != USB_MODE_DUALSENSE_EDGE && mode != USB_MODE_SINPUT;
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
            // SInput has no Steam shortcuts (see usb_mode.c): nothing matches GP_COUNT.
            bool shortcuts = mode != USB_MODE_SINPUT;
            switch (mapping_macro_run(&macro, map, shortcuts ? GP_MACRO_QAM : GP_COUNT, shortcuts ? GP_MACRO_SHOT : GP_COUNT,
                                      prev, in.buttons, platform_millis())) {
            // While a shortcut (Steam quick access, screenshot) plays, the
            // host sees only its buttons: nothing else pressed, sticks centered.
            case MACRO_GUIDE:
                gp = GP_BIT(GP_GUIDE);
                neutral_input(&in);
                break;
            case MACRO_GUIDE_SOUTH:
                gp = GP_BIT(GP_GUIDE) | GP_BIT(GP_SOUTH);
                neutral_input(&in);
                break;
            case MACRO_GUIDE_R1:
                gp = GP_BIT(GP_GUIDE) | GP_BIT(GP_R1);
                neutral_input(&in);
                break;
            default:
                gp = mapping_gp_buttons(&g_settings, map, ctx, &in);
                if (quick && mapping_quick_remap_held(in.buttons)) gp = 0;
                break;
            }
            if (mode == USB_MODE_XBOX360) x360_set_input(&in, ctx, gp, true);
            else if (mode == USB_MODE_GC_ADAPTER) gc_adapter_set_input(&in, ctx, gp, true);
            else if (mode == USB_MODE_SINPUT) sinput_set_input(&in, ctx, gp, true);
            else ds5_set_input(&in, ctx, gp, true);
            return;
        }
        procon_input_t out;
        // Home+A shortcut playing: only its buttons, sticks centered.
        ctrl_type_t ct = mapping_ctrl_type(s2_link_mapping_ctx());
        uint32_t macro_buttons = mapping_macro_step(&macro, &g_settings, ct, prev, in.buttons, platform_millis());
        if (macro_buttons) neutral_input(&in);
        mapping_apply(&g_settings, s2_link_mapping_ctx(), &in, &out);
        if (macro_buttons) {
            out.buttons = macro_buttons;
        } else if (!g_settings.quick_remap_off) {
            in_button_t back;
            if (mapping_quick_remap(&g_settings, ct, prev, in.buttons, &back)) {
                uint8_t o = settings_active_map(&g_settings, ct)[back];
                LOG("remap: %s -> %s", in_button_name(back), o ? out_button_name((out_button_t)o) : "nothing");
                settings_save_later();
                s2_link_haptic(S2_HAPTIC_TICK);   // feedback on the controller
            }
            // Keep the chord's buttons away from the host.
            if (mapping_quick_remap_held(in.buttons)) out.buttons = 0;
        }
        // Charge state byte is non-zero while external power is connected.
        procon_set_input(&out, true, in.battery_mv, battery_charging());
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
        sinput_set_input(NULL, NULL, 0, false);
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
    if (!s_usb_reconnect) usb_present_controller(s2_link_state() == S2_LINK_READY);
    if (s_usb_reconnect && platform_time_reached(s_usb_reconnect_at)) {
        s_usb_reconnect = false;
        tud_connect();
    }
    if (s_reboot && platform_time_reached(s_reboot_at)) {
        platform_reboot(s_reboot == 2);
    }
}

void app_core_init(void) {
    // Latch what the USB descriptors are made of before anything can change it.
    LOG("usb: mode %s%s", usb_mode_name(usb_mode_active()), usb_mode_has_mouse() ? " with the mouse" : "");
    mode_select_init(&s_msel);
    battery_reset(&g_battery);
    procon_init();
    ds5_init();
    x360_init();
    gc_adapter_init();
    sinput_init();
    s2_link_init();
}

void app_core_task(void) {
    platform_watchdog_feed();
    s2_link_task();
    update_input();
    mode_select_task();
    if (usb_mode_has_mouse()) mouse_task();
    usb_hid_task();
    switch (usb_mode_active()) {
    case USB_MODE_DUALSENSE_EDGE:
    case USB_MODE_DUALSENSE: ds5_task(); break;
    case USB_MODE_XBOX360: x360_task(); break;
    case USB_MODE_GC_ADAPTER: gc_adapter_task(); break;
    case USB_MODE_SINPUT: sinput_task(); break;
    default: procon_task(); break;
    }
    webusb_task();
    suspend_task();
    usb_watch_task();
    settings_task();
    maintenance_task();
}
