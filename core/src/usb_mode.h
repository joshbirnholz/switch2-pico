#ifndef S2P_USB_MODE_H
#define S2P_USB_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

// What the dongle presents on USB (settings: usb_mode). The mode is read at
// boot; changing it reboots the dongle so the host sees a new device.

#include <stdbool.h>
#include <stdint.h>

#include "procon.h"
#include "settings.h"

typedef struct {
    uint16_t vid, pid, bcd_device;
    uint8_t device_class;   // 0xFF for the Xbox 360 controller
    const char *manufacturer;
    const char *product;
} usb_identity_t;

usb_mode_t usb_mode_active(void);
const usb_identity_t *usb_mode_identity(void);
const char *usb_mode_name(usb_mode_t m);
void usb_mode_get_status(procon_status_t *out);
// Whether the USB mouse interface is there (latched at boot with the mode:
// the profile the dongle started with uses the Joy-Con 2 mouse).
bool usb_mode_has_mouse(void);
// False for vendor-class modes (Xbox 360).
bool usb_mode_is_hid(void);
// Name of a generic output in mode `m`, NULL if the mode doesn't have it.
const char *usb_mode_output_name(usb_mode_t m, gp_out_t g);

// The controller's interface + endpoint descriptors for the active mode.
uint16_t usb_mode_interface_desc(uint8_t *buf, uint16_t cap, uint8_t itf, uint8_t ep_in, uint8_t ep_out);
uint16_t usb_mode_interface_desc_len(void);

#ifdef __cplusplus
}
#endif

#endif
