#include "mode_select.h"

#include "s2_proto.h"

#define COMBO (S2_BTN_C | S2_BTN_HOME)

static const uint32_t SLOT_BITS[MODE_SLOT_COUNT] = {
    [MODE_SLOT_A] = S2_BTN_A,       [MODE_SLOT_B] = S2_BTN_B,
    [MODE_SLOT_X] = S2_BTN_X,       [MODE_SLOT_Y] = S2_BTN_Y,
    [MODE_SLOT_UP] = S2_BTN_UP,     [MODE_SLOT_DOWN] = S2_BTN_DOWN,
    [MODE_SLOT_LEFT] = S2_BTN_LEFT, [MODE_SLOT_RIGHT] = S2_BTN_RIGHT,
};

static const char *const SLOT_NAMES[MODE_SLOT_COUNT] = {"A", "B", "X", "Y", "Up", "Down", "Left", "Right"};

// Declared in settings.h (settings_defaults() uses it).
void settings_default_mode_slots(uint8_t slots[MODE_SLOT_COUNT]) {
    for (int i = 0; i < MODE_SLOT_COUNT; i++) slots[i] = MODE_SLOT_EMPTY;
    slots[MODE_SLOT_A] = USB_MODE_DUALSENSE_EDGE + 1;
    slots[MODE_SLOT_B] = USB_MODE_XBOX360 + 1;
    slots[MODE_SLOT_X] = USB_MODE_DUALSENSE + 1;
    slots[MODE_SLOT_Y] = USB_MODE_SWITCH_PRO + 1;
}

void mode_select_init(mode_select_t *m) {
    m->active = false;
    m->armed = true;
    m->held = false;
    m->since = 0;
    m->prev = 0;
    m->idle_since = 0;
}

bool mode_select_enabled(const uint8_t slots[MODE_SLOT_COUNT]) {
    for (int i = 0; i < MODE_SLOT_COUNT; i++) {
        if (slots[i] != MODE_SLOT_EMPTY && slots[i] <= PROFILE_MAX) return true;
    }
    return false;
}

uint32_t mode_select_slot_bit(mode_slot_t slot) {
    return slot < MODE_SLOT_COUNT ? SLOT_BITS[slot] : 0;
}

const char *mode_select_slot_name(mode_slot_t slot) {
    return slot < MODE_SLOT_COUNT ? SLOT_NAMES[slot] : "?";
}

void mode_select_cancel(mode_select_t *m) {
    m->active = false;
    m->armed = false;   // a combo still held must be released before it counts again
}

mode_select_event_t mode_select_update(mode_select_t *m, const uint8_t slots[MODE_SLOT_COUNT], uint32_t raw,
                                       uint32_t now_ms, uint8_t *chosen) {
    uint32_t prev = m->prev;
    m->prev = raw;
    bool combo = (raw & COMBO) == COMBO;
    if (!combo) {
        m->held = false;
        m->armed = true;
    } else if (!m->held) {
        m->held = true;
        m->since = now_ms;
    }

    if (!m->active) {
        if (!combo || !m->armed || !mode_select_enabled(slots)) return MODE_SELECT_NONE;
        if (now_ms - m->since < MODE_SELECT_HOLD_MS) return MODE_SELECT_NONE;
        m->active = true;
        m->armed = false;
        m->idle_since = now_ms;
        return MODE_SELECT_ENTER;
    }

    if (raw) m->idle_since = now_ms;
    // C + Home pressed again (after releasing it) leaves.
    if (combo && m->armed) {
        mode_select_cancel(m);
        return MODE_SELECT_CANCEL;
    }
    uint32_t pressed = raw & ~prev;
    for (int i = 0; i < MODE_SLOT_COUNT; i++) {
        if (!(pressed & SLOT_BITS[i])) continue;
        uint8_t v = slots[i];
        if (v == MODE_SLOT_EMPTY || v > PROFILE_MAX) continue;   // nothing there: ignore
        *chosen = (uint8_t)(v - 1);
        mode_select_cancel(m);
        return MODE_SELECT_CHOSEN;
    }
    if (!raw && now_ms - m->idle_since >= MODE_SELECT_TIMEOUT_MS) {
        mode_select_cancel(m);
        return MODE_SELECT_CANCEL;
    }
    return MODE_SELECT_NONE;
}
