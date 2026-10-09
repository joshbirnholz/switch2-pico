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
}

// One profile per mode, in mode order, so the default shortcut buttons
// (mode + 1) point at the profile of that mode.
void settings_default_profiles(ctrl_type_t type, ctrl_profiles_t *c) {
    memset(c, 0, sizeof *c);
    for (int m = 0; m < USB_MODE_COUNT; m++) settings_default_profile_for(type, (usb_mode_t)m, &c->p[m]);
    settings_default_mode_slots(c->slot);
    c->active = type == CTRL_GAMECUBE ? USB_MODE_GC_ADAPTER : USB_MODE_SWITCH_PRO;
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
    }
    if (!used) {
        settings_default_profiles(type, c);
        return;
    }
    if (c->active >= PROFILE_MAX || !c->p[c->active].used) {
        for (int i = 0; i < PROFILE_MAX; i++) {
            if (c->p[i].used) {
                c->active = (uint8_t)i;
                break;
            }
        }
    }
    for (int i = 0; i < MODE_SLOT_COUNT; i++) {
        uint8_t v = c->slot[i];
        if (v == MODE_SLOT_EMPTY) continue;
        if (v > PROFILE_MAX || !c->p[v - 1].used) c->slot[i] = MODE_SLOT_EMPTY;
    }
}
