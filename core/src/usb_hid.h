#ifndef S2P_USB_HID_H
#define S2P_USB_HID_H

#ifdef __cplusplus
extern "C" {
#endif

// Minimal HID class driver for the emulated Pro Controller.
//
// Separate IN/OUT endpoint handling, control-pipe SET_REPORT support and an
// output-report queue that lets the USB stack run in its own task (nRF52).
// Every report is exactly one 64 byte packet, like a genuine controller.

#include <stdbool.h>
#include <stdint.h>

#define USB_HID_EP_SIZE 64
#define USB_HID_MAX_IN_REPORT 64

bool usb_hid_ready(void);
bool usb_hid_ep_ready(void);
bool usb_hid_suspended(void);
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
