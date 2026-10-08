#include "battery.h"

#define TAU_MS          8000.0f   // smoothing time constant
#define CHARGE_OFFSET   120       // mV a charging cell reads above its resting voltage
#define JUMP_PCT        10        // a rise this large while discharging is believed

battery_t g_battery;

// Resting voltage -> level, typical single-cell Li-ion / Li-po under a light load.
static const struct {
    uint16_t mv;
    uint8_t pct;
} CURVE[] = {
    {3400, 0}, {3500, 5}, {3600, 12}, {3650, 20}, {3700, 30}, {3750, 42},
    {3800, 53}, {3850, 63}, {3900, 72}, {3950, 80}, {4000, 86}, {4050, 91},
    {4100, 96}, {4150, 100},
};

uint8_t battery_mv_to_percent(uint16_t mv) {
    const int n = sizeof CURVE / sizeof CURVE[0];
    if (mv <= CURVE[0].mv) return 0;
    if (mv >= CURVE[n - 1].mv) return 100;
    for (int i = 1; i < n; i++) {
        if (mv < CURVE[i].mv) {
            int span = CURVE[i].mv - CURVE[i - 1].mv;
            int d = (int)mv - CURVE[i - 1].mv;
            return (uint8_t)(CURVE[i - 1].pct + (CURVE[i].pct - CURVE[i - 1].pct) * d / span);
        }
    }
    return 100;
}

void battery_reset(battery_t *b) {
    b->mv = 0;
    b->last_ms = 0;
    b->pct = 100;
    b->charging = false;
}

void battery_update(battery_t *b, uint16_t mv, bool charging, uint32_t now_ms) {
    if (mv < 2500 || mv > 5000) return;   // no reading
    bool was_charging = b->charging;
    b->charging = charging;
    if (b->mv == 0 || charging != was_charging) {
        // First sample, or the charger was (dis)connected: the voltage steps.
        b->mv = (float)mv;
        b->last_ms = now_ms;
        uint16_t rest = charging && mv > CHARGE_OFFSET ? (uint16_t)(mv - CHARGE_OFFSET) : mv;
        b->pct = battery_mv_to_percent(rest);
        return;
    }
    float dt = (float)(now_ms - b->last_ms);
    b->last_ms = now_ms;
    if (dt > 60000.0f) dt = 60000.0f;
    b->mv += ((float)mv - b->mv) * (dt / (TAU_MS + dt));
    uint16_t rest = (uint16_t)(b->mv + 0.5f);
    if (charging) rest = rest > CHARGE_OFFSET ? (uint16_t)(rest - CHARGE_OFFSET) : 0;
    uint8_t p = battery_mv_to_percent(rest);
    if (charging) {
        if (p > b->pct || b->pct - p >= JUMP_PCT) b->pct = p;   // only rises while charging
    } else {
        if (p < b->pct || p - b->pct >= JUMP_PCT) b->pct = p;   // only drops while discharging
    }
}

uint8_t battery_percent(void) {
    return g_battery.mv == 0 ? 100 : g_battery.pct;
}

bool battery_charging(void) {
    return g_battery.charging;
}
