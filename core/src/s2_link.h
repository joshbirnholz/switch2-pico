#ifndef S2P_S2_LINK_H
#define S2P_S2_LINK_H

#ifdef __cplusplus
extern "C" {
#endif

// Bluetooth LE central that finds, pairs with and talks to a Nintendo Switch 2
// controller (Pro Controller 2, NSO GameCube controller) using BTstack.

#include <stdbool.h>
#include <stdint.h>

#include "hd_rumble.h"
#include "mapping.h"
#include "s2_proto.h"

typedef enum {
    S2_LINK_OFF = 0,
    S2_LINK_SCANNING,
    S2_LINK_CONNECTING,
    S2_LINK_DISCOVERING,
    S2_LINK_INITIALISING,
    S2_LINK_READY,
} s2_link_state_t;

typedef struct {
    s2_link_state_t state;
    uint8_t addr[6];            // controller address (big-endian), valid when connected
    uint16_t pid;
    char serial[17];
    uint16_t battery_mv;
    uint8_t charge_state;
    float report_rate_hz;
    uint16_t conn_interval;     // units of 1.25 ms
    uint16_t mtu;
    bool paired_this_session;
    bool pairing_ok;
    uint8_t gyro_range_detected; // gyro_range_t actually in use
    int8_t last_rssi;
    uint32_t reports;
    uint32_t rumble_packets;
    bool gyro_cal_busy;
} s2_link_info_t;

void s2_link_init(void);
void s2_link_task(void);

s2_link_state_t s2_link_state(void);
const char *s2_link_state_name(s2_link_state_t st);
void s2_link_get_info(s2_link_info_t *out);

// Latest input; returns false while not connected. `seq` increments per report.
bool s2_link_get_input(s2_input_t *out, uint32_t *seq);
const mapping_ctx_t *s2_link_mapping_ctx(void);

// Rumble from the host, decoded (see hd_rumble.h).
void s2_link_rumble_submit(const rumble_sample_t *left, int nl, const rumble_sample_t *right, int nr);
void s2_link_test_rumble(void);
// Play classic two-motor rumble (see rumble_from_motors) for `ms`.
void s2_link_test_motors(uint8_t left_strong, uint8_t right_weak, uint16_t ms);
// Play one of the controller's built-in vibration samples (1..7; 1 = buzz,
// 3 = the "ba-thump" it plays on connecting, 4 = the pairing "ka-chink").
void s2_link_play_sample(uint8_t sample);

// Short feedback effects, played on the HD rumble actuators over whatever the
// host is sending (which resumes afterwards). Controllers without them
// (GameCube) get the built-in buzz instead.
typedef enum {
    S2_HAPTIC_TICK,        // a crisp tick: shortcut accepted (GL/GR remap)
    S2_HAPTIC_BA_THUMP,    // two low pulses: mode selection started
    S2_HAPTIC_THUMP,       // one firm pulse: mode chosen
    S2_HAPTIC_COUNT
} s2_haptic_t;
void s2_link_haptic(s2_haptic_t effect);
const char *s2_link_haptic_name(s2_haptic_t effect);

void s2_link_set_player_leds(uint8_t pattern);
// Show `pattern` instead of the player LEDs until cleared with -1.
void s2_link_set_led_override(int pattern);

void s2_link_disconnect(void);
// Disconnect so the controller can go to sleep (e.g. after inactivity): its
// own reconnection attempts right afterwards are ignored; a button press
// later on reconnects as usual.
void s2_link_let_controller_sleep(void);

// Pairing window (settings.pair_button): while it is open, a controller in
// pairing mode is accepted (and only that: a connected controller is dropped
// so a new one can be found). Outside it such controllers are ignored.
// With pair_button off, pairing is always open (s2_link_pairing_open()
// returns false: there is no window to show).
void s2_link_start_pairing(uint32_t ms);
void s2_link_stop_pairing(void);
bool s2_link_pairing_open(void);
uint32_t s2_link_pairing_left_ms(void);
// Forget one paired controller (BD_ADDR as printed, big-endian), or all with NULL.
void s2_link_forget(const uint8_t *addr);
// Pause/resume scanning and connecting (e.g. while the USB host is asleep).
void s2_link_set_paused(bool paused);

void s2_link_start_gyro_calibration(void);

// Lower the scan duty cycle (used while the Wi-Fi access point is on).
void s2_link_set_low_duty_scan(bool low);

// Where the last failed connection attempt stopped:
//   1 = link not established, 2 = GATT discovery, 3 = first commands /
//   Nintendo pairing, 4 = controller initialisation. `reason` is the HCI error/disconnect code.
bool s2_link_last_failure(uint8_t *stage, uint8_t *reason, uint32_t *age_ms);

// Hook: a bonded controller advertised while we were not connected.
void s2_link_hook_controller_seen(void);
// Hook: the link became ready / was lost.
void s2_link_hook_connection_changed(bool connected);
// Hook: controller info (colours etc.) was read.
void s2_link_hook_controller_colors(const uint8_t rgb[12]);

#ifdef __cplusplus
}
#endif

#endif
