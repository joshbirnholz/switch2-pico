// Profiles per controller type (see profile_t in settings.h). Pure logic,
// covered by test/test_main.c.

#include <stdio.h>
#include <string.h>

#include "settings.h"

static const char *const MODE_PROFILE_NAMES[USB_MODE_COUNT] = {
    [USB_MODE_SWITCH_PRO] = "Switch Pro",
    [USB_MODE_DUALSENSE_EDGE] = "DualSense Edge",
    [USB_MODE_DUALSENSE] = "DualSense",
    [USB_MODE_XBOX360] = "Xbox 360",
    [USB_MODE_GC_ADAPTER] = "GameCube adapter",
    [USB_MODE_SINPUT] = "SInput",
};

const char *settings_mode_profile_name(usb_mode_t mode) {
    return mode < USB_MODE_COUNT ? MODE_PROFILE_NAMES[mode] : "Profile";
}

static void default_map(ctrl_type_t type, usb_mode_t mode, uint8_t map[IN_COUNT]) {
    if (mode == USB_MODE_SWITCH_PRO) settings_default_button_map(type, map);
    else settings_default_mode_map(type, mode, map);
}

void settings_default_profile_for(ctrl_type_t type, usb_mode_t mode, profile_t *p) {
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->usb_mode = (uint8_t)mode;
    snprintf(p->name, sizeof p->name, "%s", settings_mode_profile_name(mode));
    default_map(type, mode, p->map);
    p->stick_deadzone_pct = 6;
    p->stick_outer_pct = 95;
    p->swap_sticks = 0;
    p->trigger_threshold = 120;
    p->rumble_enabled = 1;
    p->rumble_strength_pct = 100;
    if (ctrl_is_joycon(type)) {
        // The mouse stays off until chosen; these are its starting options.
        p->mouse_speed_pct = 100;
    }
}

// One profile per mode, on the buttons the shortcut used before profiles:
// Y Switch Pro, A DualSense Edge, X DualSense, B Xbox 360, D-pad up GameCube
// adapter; then D-pad down SInput; the rest empty.
void settings_default_profiles(ctrl_type_t type, ctrl_profiles_t *c) {
    static const uint8_t BUTTON_OF_MODE[USB_MODE_COUNT] = {
        [USB_MODE_SWITCH_PRO] = MODE_SLOT_Y, [USB_MODE_DUALSENSE_EDGE] = MODE_SLOT_A,
        [USB_MODE_DUALSENSE] = MODE_SLOT_X,  [USB_MODE_XBOX360] = MODE_SLOT_B,
        [USB_MODE_GC_ADAPTER] = MODE_SLOT_UP, [USB_MODE_SINPUT] = MODE_SLOT_DOWN,
    };
    memset(c, 0, sizeof *c);
    for (int m = 0; m < USB_MODE_COUNT; m++) settings_default_profile_for(type, (usb_mode_t)m, &c->p[BUTTON_OF_MODE[m]]);
    c->active = BUTTON_OF_MODE[type == CTRL_GAMECUBE ? USB_MODE_GC_ADAPTER : USB_MODE_SWITCH_PRO];
    // A single Joy-Con 2 has no shortcut: its profiles are just numbered
    // (chosen on the page), one per mode in mode order, the first in use.
    if (!profiles_on_buttons(type)) {
        memset(c, 0, sizeof *c);
        for (int m = 0; m < USB_MODE_COUNT; m++) settings_default_profile_for(type, (usb_mode_t)m, &c->p[m]);
        c->active = 0;
    }
}

// Profiles as stored before ext_rev 14: maps of IN_COUNT_V1 inputs.
typedef struct {
    uint8_t used, usb_mode;
    char name[PROFILE_NAME_LEN];
    uint8_t map[IN_COUNT_V1];
    uint8_t stick_deadzone_pct, stick_outer_pct, swap_sticks, trigger_threshold;
    uint8_t rumble_enabled, rumble_strength_pct;
    uint8_t mouse_src, mouse_speed_pct, mouse_flags;
    uint8_t reserved[5];
} profile_v13_t;
typedef struct {
    profile_v13_t p[PROFILE_MAX];
    uint8_t slot[MODE_SLOT_COUNT];
    uint8_t active;
    uint8_t reserved[3];
} ctrl_profiles_v13_t;
_Static_assert(sizeof(profile_v13_t) == 57, "stored profile layout");
_Static_assert(sizeof(ctrl_profiles_v13_t) == CTRL_PROFILES_V13_SIZE, "stored profiles layout");

void settings_profiles_from_v13(const uint8_t *raw, int n, ctrl_profiles_t *out) {
    for (int t = 0; t < n; t++) {
        ctrl_profiles_v13_t old;
        memcpy(&old, raw + (size_t)t * sizeof old, sizeof old);
        ctrl_profiles_t *c = &out[t];
        memset(c, 0, sizeof *c);
        for (int i = 0; i < PROFILE_MAX; i++) {
            const profile_v13_t *o = &old.p[i];
            profile_t *p = &c->p[i];
            p->used = o->used;
            p->usb_mode = o->usb_mode;
            memcpy(p->name, o->name, sizeof p->name);
            memcpy(p->map, o->map, IN_COUNT_V1);   // the inputs added since: unassigned (0)
            p->stick_deadzone_pct = o->stick_deadzone_pct;
            p->stick_outer_pct = o->stick_outer_pct;
            p->swap_sticks = o->swap_sticks;
            p->trigger_threshold = o->trigger_threshold;
            p->rumble_enabled = o->rumble_enabled;
            p->rumble_strength_pct = o->rumble_strength_pct;
            p->mouse_src = o->mouse_src;
            p->mouse_speed_pct = o->mouse_speed_pct;
            p->mouse_flags = o->mouse_flags;
        }
        memcpy(c->slot, old.slot, sizeof c->slot);
        c->active = old.active;
    }
}

// Numbered profiles (single Joy-Con 2): the first one in use takes over,
// and settings_profiles_numbered() packs them from the start.
void settings_profiles_numbered(ctrl_profiles_t *c) {
    ctrl_profiles_t old = *c;
    int n = 0;
    memset(c->p, 0, sizeof c->p);
    c->active = 0;
    for (int i = 0; i < PROFILE_MAX; i++) {
        if (!old.p[i].used) continue;
        if (i == old.active) c->active = (uint8_t)n;
        c->p[n++] = old.p[i];
    }
}

// When the profile in use is gone, the first of these takes over.
static const uint8_t FALLBACK_ORDER[PROFILE_MAX] = {
    MODE_SLOT_Y, MODE_SLOT_A, MODE_SLOT_X, MODE_SLOT_B, MODE_SLOT_UP, MODE_SLOT_RIGHT, MODE_SLOT_DOWN, MODE_SLOT_LEFT,
};
// Free buttons for profiles no button selected (D-pad first).
static const uint8_t FREE_ORDER[PROFILE_MAX] = {
    MODE_SLOT_UP, MODE_SLOT_RIGHT, MODE_SLOT_DOWN, MODE_SLOT_LEFT, MODE_SLOT_X, MODE_SLOT_A, MODE_SLOT_B, MODE_SLOT_Y,
};

void settings_profiles_to_buttons(ctrl_profiles_t *c) {
    ctrl_profiles_t old = *c;
    memset(c, 0, sizeof *c);
    bool placed[PROFILE_MAX] = {false};
    int active = -1;
    // Each button gets the profile it selected (one picked by two buttons is
    // copied, so both keep working).
    for (int b = 0; b < PROFILE_MAX; b++) {
        uint8_t v = old.slot[b];
        if (v == MODE_SLOT_EMPTY || v > PROFILE_MAX || !old.p[v - 1].used) continue;
        c->p[b] = old.p[v - 1];
        if (v - 1 == old.active && (!placed[v - 1] || active < 0)) active = b;
        placed[v - 1] = true;
    }
    // Profiles no button selected: the next free buttons (dropped if none left).
    for (int i = 0; i < PROFILE_MAX; i++) {
        if (!old.p[i].used || placed[i]) continue;
        for (int k = 0; k < PROFILE_MAX; k++) {
            uint8_t b = FREE_ORDER[k];
            if (c->p[b].used) continue;
            c->p[b] = old.p[i];
            if (i == old.active) active = b;
            break;
        }
    }
    c->active = active >= 0 ? (uint8_t)active : PROFILE_MAX;   // sanitize picks one
}

static uint8_t clamp(uint8_t v, uint8_t lo, uint8_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static bool map_valid(const profile_t *p) {
    uint8_t limit = p->usb_mode == USB_MODE_SWITCH_PRO ? OUT_COUNT : GP_COUNT;
    for (int i = 0; i < IN_COUNT; i++) {
        if (p->map[i] >= limit) return false;
    }
    return true;
}

void settings_sanitize_profiles(ctrl_type_t type, ctrl_profiles_t *c) {
    int used = 0;
    for (int i = 0; i < PROFILE_MAX; i++) {
        profile_t *p = &c->p[i];
        if (p->used != 1) {
            memset(p, 0, sizeof *p);
            continue;
        }
        used++;
        if (p->usb_mode >= USB_MODE_COUNT) p->usb_mode = USB_MODE_SWITCH_PRO;
        p->name[PROFILE_NAME_LEN - 1] = 0;
        if (!p->name[0]) snprintf(p->name, sizeof p->name, "Profile %d", i + 1);
        if (!map_valid(p)) default_map(type, (usb_mode_t)p->usb_mode, p->map);
        p->stick_deadzone_pct = clamp(p->stick_deadzone_pct, 0, 40);
        p->stick_outer_pct = clamp(p->stick_outer_pct, 50, 100);
        p->swap_sticks = p->swap_sticks ? 1 : 0;
        p->rumble_enabled = p->rumble_enabled ? 1 : 0;
        p->rumble_strength_pct = clamp(p->rumble_strength_pct, 0, 200);
        if (ctrl_is_joycon(type)) {
            // On or off (earlier versions named a side: either is on).
            p->mouse_src = p->mouse_src ? MOUSE_ON : MOUSE_OFF;
            p->mouse_speed_pct = clamp(p->mouse_speed_pct ? p->mouse_speed_pct : 100, 10, 250);
            p->mouse_flags &= MOUSE_FLAGS_ALL;
        } else {
            p->mouse_src = p->mouse_speed_pct = p->mouse_flags = 0;
        }
    }
    if (!used) {
        settings_default_profiles(type, c);
        return;
    }
    // The profile in use was cleared (or never set): another one takes over.
    if (c->active >= PROFILE_MAX || !c->p[c->active].used) {
        for (int k = 0; k < PROFILE_MAX; k++) {
            int i = profiles_on_buttons(type) ? FALLBACK_ORDER[k] : k;
            if (c->p[i].used) {
                c->active = (uint8_t)i;
                break;
            }
        }
    }
    memset(c->slot, 0, sizeof c->slot);
}
