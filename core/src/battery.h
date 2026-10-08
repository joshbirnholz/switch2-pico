#ifndef S2P_BATTERY_H
#define S2P_BATTERY_H

// Controller battery level from the voltage the Switch 2 controller reports.
// A lithium cell's voltage isn't linear in its charge (flat in the middle,
// steep at both ends), dips under load (rumble) and reads high while
// charging, so the level is: smoothed voltage -> typical discharge curve,
// held steady while discharging (it only drops, apart from a clear jump).
// Pure logic; covered by test/test_main.c.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float mv;            // smoothed voltage (0: no sample yet)
    uint32_t last_ms;
    uint8_t pct;         // level shown, 0..100
    bool charging;
} battery_t;

// Typical lithium-ion level for a resting cell voltage, 0..100.
uint8_t battery_mv_to_percent(uint16_t mv);

void battery_reset(battery_t *b);
// Feed every report. `charging`: external power connected.
void battery_update(battery_t *b, uint16_t mv, bool charging, uint32_t now_ms);

// Shared instance fed by app_core.c; 100 / not charging until data arrives.
extern battery_t g_battery;
uint8_t battery_percent(void);
bool battery_charging(void);

#ifdef __cplusplus
}
#endif

#endif
