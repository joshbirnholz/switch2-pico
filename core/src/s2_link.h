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

typedef enum {
    NFC_SRC_HOST = 1,       // the USB host asked the emulated MCU to poll
    NFC_SRC_WEB = 2,        // the configuration page asked for a scan
} nfc_source_t;

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
    bool nfc_active;
    uint8_t nfc_raw_state;
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

void s2_link_set_player_leds(uint8_t pattern);

void s2_link_nfc_request(nfc_source_t src, bool enable);

void s2_link_disconnect(void);
void s2_link_forget(void);
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
