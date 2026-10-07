#ifndef S2P_MCU_NFC_H
#define S2P_MCU_NFC_H

// Emulation of the NFC/IR microcontroller found in a Switch 1 Pro Controller,
// limited to what is needed to read (and accept writes to) NTAG215 amiibo.
//
// The host talks to the MCU with:
//   * subcommand 0x22 (set MCU state: suspend / resume)
//   * subcommand 0x21 (set MCU configuration: standby / NFC / IR)
//   * output report 0x11 (MCU requests; 0x02 = NFC commands)
// and receives 313 byte MCU packets at the end of each 0x31 input report.
//
// The packet formats follow the public reverse engineering (dekuNukem,
// CTCaer's jc_toolkit, joycontrol). Pure C apart from the amiibo store.

#include <stdbool.h>
#include <stdint.h>

#define MCU_PACKET_LEN 313
#define MCU_CONFIG_REPLY_LEN 34

typedef enum {
    MCU_SUSPENDED = 0x00,
    MCU_READY = 0x01,
    MCU_NFC = 0x04,
} mcu_power_t;

typedef enum {
    NFC_NONE = 0x00,
    NFC_POLL = 0x01,
    NFC_PENDING_READ = 0x02,
    NFC_WRITING = 0x03,
    NFC_AWAITING_WRITE = 0x04,
    NFC_PROCESSING_WRITE = 0x05,
    NFC_POLL_AGAIN = 0x09,
} nfc_state_t;

#define MCU_QUEUE_LEN 4
#define MCU_WRITE_BUF 600

typedef struct {
    mcu_power_t power;
    nfc_state_t nfc_state;
    bool polling;                 // host asked us to look for tags
    int counter;
    uint8_t last_uid[7];
    bool have_last_uid;
    int pending_remove;           // report an empty tag for this many polls
    uint8_t ack_seq;
    uint8_t queue[MCU_QUEUE_LEN][MCU_PACKET_LEN];
    int q_head, q_count;
    uint8_t write_buf[MCU_WRITE_BUF];
    int write_len;
} mcu_t;

// Hooks implemented by the firmware (weak defaults do nothing).
void mcu_hook_polling_changed(bool polling);
void mcu_hook_tag_written(void);

uint8_t mcu_crc8(const uint8_t *data, int len);

void mcu_reset(mcu_t *m);
void mcu_enter_report_mode_31(mcu_t *m);
// Subcommand 0x22.
void mcu_set_power(mcu_t *m, uint8_t state);
// Subcommand 0x21; fills the reply data that follows the subcommand id.
void mcu_set_config(mcu_t *m, const uint8_t *args, int len, uint8_t reply[MCU_CONFIG_REPLY_LEN]);
// Output report 0x11, payload after the report id:
// [timer][8 rumble bytes][mcu subcommand][data...]
void mcu_handle_request(mcu_t *m, const uint8_t *payload, int len);
// Next packet to place in a 0x31 input report.
void mcu_next_packet(mcu_t *m, uint8_t out[MCU_PACKET_LEN]);

#endif
