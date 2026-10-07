#ifndef S2P_S2_TRANSPORT_H
#define S2P_S2_TRANSPORT_H

// Bluetooth LE transport used by the shared controller logic (s2_link.c).
// Each board implements it on its own Bluetooth stack:
//   pico/s2_ble_btstack.c            BTstack on the CYW43439
//   nrf52/switch2_nrf/s2_ble_bluefruit.cpp  Bluefruit / SoftDevice S140
//
// The transport owns the radio: scanning, connecting, GATT discovery of the
// Switch 2 HID service and enabling notifications. It must never start SMP
// pairing (Switch 2 controllers drop the link if it is attempted).
//
// All s2c_* callbacks must be invoked from the main loop context (transports
// running on another thread queue events and deliver them from s2t_task()).

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    S2T_CHAR_COMMAND = 0,   // 649d4ac9-... (write without response)
    S2T_CHAR_VIBRATION,     // controller specific rumble output (optional)
} s2t_char_t;

typedef enum {
    S2T_WRITE_OK = 0,
    S2T_WRITE_BUSY,         // no TX buffer right now, try again later
    S2T_WRITE_ERROR,        // will never succeed (too long, no such characteristic)
} s2t_write_result_t;

// ---- implemented by the transport ----
void s2t_init(void);
void s2t_task(void);
bool s2t_ready(void);                       // stack is up, scanning possible
void s2t_start_scan(bool low_duty);
void s2t_stop_scan(void);
// Connect to a controller. Address is big-endian (as printed).
bool s2t_connect(const uint8_t addr[6], uint8_t addr_type);
void s2t_cancel_connect(void);
void s2t_disconnect(void);
s2t_write_result_t s2t_write(s2t_char_t ch, const uint8_t *data, uint16_t len);
bool s2t_has_char(s2t_char_t ch);
// Enable notifications on the input report characteristic; completes with
// s2c_on_input_enabled().
void s2t_enable_input(void);
void s2t_local_address(uint8_t out[6]);     // big-endian
uint16_t s2t_mtu(void);

// ---- implemented by the shared core (s2_link.c) ----
void s2c_on_stack_ready(void);
void s2c_on_advertisement(const uint8_t addr[6], uint8_t addr_type, int8_t rssi,
                          const uint8_t *adv_data, uint16_t adv_len);
void s2c_on_link_up(uint16_t conn_interval);
void s2c_on_connect_failed(uint8_t reason);
// GATT discovery finished and command responses are subscribed (or failed).
void s2c_on_gatt_ready(bool ok, const char *error);
void s2c_on_command_response(const uint8_t *data, uint16_t len);
void s2c_on_input_report(const uint8_t *data, uint16_t len);
void s2c_on_input_enabled(bool ok);
void s2c_on_conn_interval(uint16_t conn_interval);
void s2c_on_disconnected(uint8_t reason);

#ifdef __cplusplus
}
#endif

#endif
