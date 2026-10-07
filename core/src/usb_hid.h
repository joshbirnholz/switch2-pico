#ifndef S2P_USB_HID_H
#define S2P_USB_HID_H

#ifdef __cplusplus
extern "C" {
#endif

// Minimal HID class driver for the emulated Pro Controller.
//
// TinyUSB's stock HID class sizes its IN and OUT transfers with the same
// CFG_TUD_HID_EP_BUFSIZE. We need 64 byte OUT transfers (every output report
// is exactly one packet) but a 362 byte IN transfer for the 0x31 NFC report,
// so this driver keeps the two independent.

#include <stdbool.h>
#include <stdint.h>

#define USB_HID_EP_SIZE 64
#define USB_HID_MAX_IN_REPORT 384

bool usb_hid_ready(void);
// Send report `id` followed by `len` bytes of data.
bool usb_hid_send(uint8_t id, const uint8_t *data, uint16_t len);
bool usb_hid_mounted(void);

// Output reports are queued by the USB stack (which may run in its own task
// or interrupt context) and delivered to usb_hid_on_output() from here.
// Call from the main loop.
void usb_hid_task(void);

// Implemented by the application (procon.c).
// Called from usb_hid_task() in main loop context.
void usb_hid_on_output(const uint8_t *buf, uint16_t len, bool via_control, uint8_t control_report_id);
// Implemented in usb_descriptors.c.
const uint8_t *usb_hid_report_descriptor(uint16_t *len);

#ifdef __cplusplus
}
#endif

#endif
