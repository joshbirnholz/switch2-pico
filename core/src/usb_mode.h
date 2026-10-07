#ifndef S2P_USB_MODE_H
#define S2P_USB_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

// What the dongle presents on USB (settings: usb_mode). The mode is read at
// boot; changing it reboots the dongle so the host sees a new device.

#include <stdint.h>

#include "procon.h"
#include "settings.h"

typedef struct {
    uint16_t vid, pid, bcd_device;
    const char *manufacturer;
    const char *product;
} usb_identity_t;

usb_mode_t usb_mode_active(void);
const usb_identity_t *usb_mode_identity(void);
const char *usb_mode_name(usb_mode_t m);
void usb_mode_get_status(procon_status_t *out);

#ifdef __cplusplus
}
#endif

#endif
