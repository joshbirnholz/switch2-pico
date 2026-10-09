#ifndef S2P_MODE_SELECT_H
#define S2P_MODE_SELECT_H

// Profile shortcut on the controller: hold C + Home to enter selection, then
// press a face button or D-pad direction that has a profile assigned
// (ctrl_profiles_t.slot). C + Home again, or a few seconds without any
// button held, leaves without a change. Buttons without a profile are ignored.
//
// Pure logic (no hardware): app_core.c feeds it the raw Switch 2 buttons and
// acts on the events; covered by test/test_main.c.

#include <stdbool.h>
#include <stdint.h>

#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MODE_SELECT_HOLD_MS    1500   // C + Home held this long enters selection
#define MODE_SELECT_TIMEOUT_MS 5000   // no button held this long leaves it

typedef enum {
    MODE_SELECT_NONE = 0,
    MODE_SELECT_ENTER,     // selection started
    MODE_SELECT_CANCEL,    // left without a choice (C + Home again, or timeout)
    MODE_SELECT_CHOSEN,    // a button with a profile was pressed: *chosen is its index
} mode_select_event_t;

typedef struct {
    bool active;
    bool armed;            // the combo was released since it last fired
    bool held;             // the combo is down
    uint32_t since;        // when it went down
    uint32_t prev;         // raw buttons at the previous update
    uint32_t idle_since;   // last time any button was held (while active)
} mode_select_t;

// A single Joy-Con 2 (sideways) has no shortcut: its profile is chosen on
// the configuration page (its profiles are numbered, not on buttons).
static inline bool mode_select_for(ctrl_type_t t) {
    return t != CTRL_JOYCON_L && t != CTRL_JOYCON_R;
}

void mode_select_init(mode_select_t *m);
// True if any button has a profile assigned (otherwise the shortcut is off).
bool mode_select_enabled(const uint8_t slots[MODE_SLOT_COUNT]);
// Raw Switch 2 button bit of a slot.
uint32_t mode_select_slot_bit(mode_slot_t slot);
const char *mode_select_slot_name(mode_slot_t slot);
// Call with every new button state (and periodically, for the timeout).
mode_select_event_t mode_select_update(mode_select_t *m, const uint8_t slots[MODE_SLOT_COUNT], uint32_t raw,
                                       uint32_t now_ms, uint8_t *chosen);
// Leave selection (e.g. the controller disconnected); no event.
void mode_select_cancel(mode_select_t *m);

#ifdef __cplusplus
}
#endif

#endif
