#ifndef S2P_APP_CORE_H
#define S2P_APP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call after settings_init() and after the USB stack is up.
void app_core_init(void);
// Call continuously from the main loop.
void app_core_task(void);

// Helper for button-combo hotkeys: true once when `combo` (Switch 2 button
// bits) has been held for `hold_ms`.
bool app_combo_held(uint32_t combo, uint32_t hold_ms, bool *armed, uint32_t *since);

// Leave USB mode selection (C + Home) if it is active, without a change.
void app_mode_select_cancel(void);

#ifdef __cplusplus
}
#endif

#endif
