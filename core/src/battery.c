#include "battery.h"

#define WINDOW_MS       4000      // highest reading per window
#define TAU_MS          60000.0f  // smoothing of the window highs
#define TAU_SETTLE_MS   8000.0f   // ... while settling
#define SETTLE_MS       60000     // level follows the estimate freely this long
#define STEP_MS         20000     // then at most 1 % per this
#define CHG_DEBOUNCE_MS 3000      // charger flag must hold this long
#define CHARGE_OFFSET   120       // mV a charging cell reads above its resting voltage
#define STATE_FULL      0x20      // charge state: on external power, charged

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
    *b = (battery_t){0};
    b->pct = 100;
}

static bool reached(uint32_t now, uint32_t t) {
    return (int32_t)(now - t) >= 0;
}

static uint8_t level_of(const battery_t *b, uint16_t window_high) {
    int rest = window_high - (b->charging ? CHARGE_OFFSET : 0);
    return battery_mv_to_percent(rest > 0 ? (uint16_t)rest : 0);
}

// A window ended with this highest reading.
static void window_done(battery_t *b, uint16_t high, uint32_t now) {
    int rest = high - (b->charging ? CHARGE_OFFSET : 0);
    if (rest < 0) rest = 0;
    bool settling = !reached(now, b->settle_until);
    if (!b->have) {
        b->mv = (float)rest;
        b->have = true;
    } else {
        float tau = settling ? TAU_SETTLE_MS : TAU_MS;
        b->mv += ((float)rest - b->mv) * (WINDOW_MS / (tau + WINDOW_MS));
    }
    uint8_t target = battery_mv_to_percent((uint16_t)(b->mv + 0.5f));
    if (b->full) target = 100;
    if (settling) {
        b->pct = target;
        b->last_step = now;
        return;
    }
    if (!reached(now, b->last_step + STEP_MS)) return;
    if (b->charging ? target > b->pct : target < b->pct) {
        b->pct = (uint8_t)(b->charging ? b->pct + 1 : b->pct - 1);
        b->last_step = now;
    }
}

void battery_set_level(battery_t *b, uint8_t level, bool external_power, bool charging) {
    if (level > 9) level = 9;
    b->has_level = true;
    b->level = level;
    b->pct = (uint8_t)(level * 100 / 9);
    b->charging = external_power;
    b->full = external_power && !charging;
    if (b->full) b->pct = 100;
}

void battery_update(battery_t *b, uint16_t mv, uint8_t state, uint32_t now) {
    b->last_state = state;
    if (mv < 2500 || mv > 5000) return;   // no reading
    b->last_mv = mv;
    if (b->has_level) return;   // the controller's own level is better

    // Charger flag, debounced. A change steps the voltage (CHARGE_OFFSET):
    // start over with a new settling period.
    bool chg = state != 0;
    if (chg != b->chg_raw) {
        b->chg_raw = chg;
        b->chg_since = now;
    }
    if (!b->started) {
        b->started = true;
        b->charging = chg;
        b->settle_until = now + SETTLE_MS;
    } else if (chg != b->charging && reached(now, b->chg_since + CHG_DEBOUNCE_MS)) {
        b->charging = chg;
        b->have = false;
        b->win_max = 0;
        b->settle_until = now + SETTLE_MS;
    }
    b->full = b->charging && state == STATE_FULL;
    if (b->full) b->pct = 100;

    if (b->win_max == 0) {
        b->win_start = now;
        b->win_max = b->win_min = mv;
        // A level right away, refined as windows complete.
        if (!b->have) b->pct = b->full ? 100 : level_of(b, mv);
        return;
    }
    if (mv > b->win_max) b->win_max = mv;
    if (mv < b->win_min) b->win_min = mv;
    if (reached(now, b->win_start + WINDOW_MS)) {
        window_done(b, b->win_max, now);
        b->win_start = now;
        b->win_max = b->win_min = mv;
    }
}

uint8_t battery_percent(void) {
    return g_battery.pct;
}

bool battery_charging(void) {
    return g_battery.charging && !g_battery.full;
}
