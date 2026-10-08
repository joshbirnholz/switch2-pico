#ifndef S2P_GC_ADAPTER_H
#define S2P_GC_ADAPTER_H

#ifdef __cplusplus
extern "C" {
#endif

// Emulated Nintendo GameCube controller adapter for Wii U / Switch (WUP-028,
// 057E:0337), as used natively by the Switch and Wii U and by Dolphin,
// Slippi and Steam. One HID interface: the host sends 0x13 (start) and 0x11
// + 4 bytes (rumble per port, 1 = on); the adapter sends 0x21 + 4 ports x 9
// bytes: status, buttons, main stick, C-stick, analog L / R. The controller
// sits in port 1; the other ports are empty.

#include <stdbool.h>
#include <stdint.h>

#include "mapping.h"
#include "procon.h"
#include "s2_proto.h"

void gc_adapter_init(void);
void gc_adapter_task(void);
// `gp`: mapped outputs (GP_BIT, see usb_mode_output_name for the GC names).
void gc_adapter_set_input(const s2_input_t *in, const mapping_ctx_t *ctx, uint32_t gp, bool connected);
void gc_adapter_on_output(const uint8_t *buf, uint16_t len);
void gc_adapter_get_status(procon_status_t *out);
const uint8_t *gc_adapter_report_descriptor(uint16_t *len);

// Pure helpers (unit tested).
#define GC_REPORT_LEN 37   // 0x21 + 4 ports x 9 bytes
typedef struct {
    bool connected;
    uint32_t gp;
    uint8_t stick[2], cstick[2];   // 128 = center, Y up
    uint8_t l, r;                  // analog triggers
} gc_port_t;
void gc_build_report(const gc_port_t *port1, uint8_t out[GC_REPORT_LEN]);
// 12-bit stick value (S1_STICK_CENTER +- S1_STICK_RANGE) -> GameCube byte.
uint8_t gc_axis(uint16_t v12);

typedef struct {
    bool start;      // 0x13
    bool rumble;     // 0x11
    bool rumble_on;  // port 1
} gc_output_t;
bool gc_parse_output(const uint8_t *buf, uint16_t len, gc_output_t *out);

#ifdef __cplusplus
}
#endif

#endif
