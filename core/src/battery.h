#ifndef S2P_BATTERY_H
#define S2P_BATTERY_H

// Controller battery level from the voltage the Switch 2 controller reports
// (input report 0x05: mV at 0x1F, charge state at 0x21). A lithium cell's
// voltage isn't linear in its charge (flat in the middle, steep at both
// ends), sags under load (rumble, radio) and reads high while charging. So:
//  - each 4 s window keeps its highest reading, the one closest to the
//    cell's resting voltage (load only pulls it down);
//  - those are smoothed slowly and mapped through a typical discharge curve;
//  - the level shown settles freely for the first minute of a connection,
//    then moves at most 1 % per 20 s and only down while discharging, only
//    up while charging;
//  - the charger flag must hold for 3 s before it counts; state 0x20 (full
//    on external power) shows 100 %.
// Pure logic; covered by test/test_main.c.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool started;         // a reading arrived since the reset
    bool have;            // a smoothed estimate exists
    float mv;             // smoothed resting-voltage estimate
    uint32_t settle_until;   // level follows the estimate freely until then
    uint32_t win_start;   // current 4 s window
    uint16_t win_max, win_min;
    uint32_t last_step;   // last 1 % step of `pct`
    bool chg_raw;         // charger flag as last reported
    uint32_t chg_since;   // ... since then
    uint8_t pct;          // level shown, 0..100
    bool charging;        // on external power (debounced); charging unless `full`
    bool full;            // on external power and charged
    uint16_t last_mv;     // last raw reading
    uint8_t last_state;   // last raw charge state
} battery_t;

// Typical lithium-ion level for a resting cell voltage, 0..100.
uint8_t battery_mv_to_percent(uint16_t mv);

void battery_reset(battery_t *b);
// Feed every report: raw voltage and charge state byte.
void battery_update(battery_t *b, uint16_t mv, uint8_t charge_state, uint32_t now_ms);

// Shared instance fed by app_core.c; 100 / not charging until data arrives.
extern battery_t g_battery;
uint8_t battery_percent(void);
bool battery_charging(void);

#ifdef __cplusplus
}
#endif

#endif
