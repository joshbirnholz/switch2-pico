#ifndef S2P_X360_H
#define S2P_X360_H

#ifdef __cplusplus
extern "C" {
#endif

// Emulated wired Xbox 360 controller (045E:028E, XInput). Not HID: a vendor
// interface (class 0xFF, subclass 0x5D, protocol 0x01) with a 20 byte input
// report and short output messages (rumble, LED ring). Windows (xusb22),
// Linux (xpad), SDL and Steam all drive it natively. No gyro, and no extra
// buttons: GL / GR / C are mapped to standard buttons (C + GL/GR + button).

#include <stdbool.h>
#include <stdint.h>

#include "mapping.h"
#include "procon.h"
#include "s2_proto.h"

void x360_init(void);
void x360_task(void);
void x360_set_input(const s2_input_t *in, const mapping_ctx_t *ctx, uint32_t gp, bool connected);
void x360_on_output(const uint8_t *buf, uint16_t len);
void x360_get_status(procon_status_t *out);

// Interface descriptor (interface, vendor XInput descriptor, 2 endpoints).
#define X360_ITF_DESC_LEN (9 + 17 + 7 + 7)
#define X360_EP_SIZE 32
void x360_interface_desc(uint8_t out[X360_ITF_DESC_LEN], uint8_t itf, uint8_t ep_in, uint8_t ep_out);

// Pure helpers (unit tested).
#define X360_INPUT_LEN 20
void x360_build_input(uint32_t gp, const uint16_t stick_l[2], const uint16_t stick_r[2], uint8_t lt, uint8_t rt,
                      uint8_t out[X360_INPUT_LEN]);

typedef struct {
    bool rumble;
    uint8_t motor_left;     // strong
    uint8_t motor_right;    // weak
    bool led;
    uint8_t player;         // 1..4, 0 = none
} x360_output_t;
bool x360_parse_output(const uint8_t *buf, uint16_t len, x360_output_t *out);

#ifdef __cplusplus
}
#endif

#endif
