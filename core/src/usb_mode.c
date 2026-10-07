#include "usb_mode.h"

#include "ds5.h"
#include "usb_hid.h"

static const usb_identity_t IDENTITIES[USB_MODE_COUNT] = {
    [USB_MODE_SWITCH_PRO] = {0x057E, 0x2009, 0x0200, "Nintendo Co., Ltd.", "Pro Controller"},
    [USB_MODE_DUALSENSE_EDGE] = {0x054C, 0x0DF2, 0x0100, "Sony Interactive Entertainment",
                                 "DualSense Edge Wireless Controller"},
};

static const char *const NAMES[USB_MODE_COUNT] = {
    [USB_MODE_SWITCH_PRO] = "switch_pro",
    [USB_MODE_DUALSENSE_EDGE] = "dualsense_edge",
};

usb_mode_t usb_mode_active(void) {
    // Latched at first use (boot): descriptors must not change under the host.
    static int mode = -1;
    if (mode < 0) mode = g_settings.usb_mode < USB_MODE_COUNT ? g_settings.usb_mode : USB_MODE_SWITCH_PRO;
    return (usb_mode_t)mode;
}

const usb_identity_t *usb_mode_identity(void) {
    return &IDENTITIES[usb_mode_active()];
}

const char *usb_mode_name(usb_mode_t m) {
    return m < USB_MODE_COUNT ? NAMES[m] : "?";
}

void usb_mode_get_status(procon_status_t *out) {
    if (usb_mode_active() == USB_MODE_DUALSENSE_EDGE) ds5_get_status(out);
    else procon_get_status(out);
}

// ---- usb_hid.h hooks --------------------------------------------------------
const uint8_t *usb_hid_report_descriptor(uint16_t *len) {
    if (usb_mode_active() == USB_MODE_DUALSENSE_EDGE) return ds5_report_descriptor(len);
    return procon_report_descriptor(len);
}

uint16_t usb_hid_get_feature(uint8_t report_id, uint8_t *buf, uint16_t len) {
    if (usb_mode_active() == USB_MODE_DUALSENSE_EDGE) return ds5_get_feature(report_id, buf, len);
    // Switch Pro: nothing meaningful; an empty report of the requested ID.
    for (uint16_t i = 0; i < len; i++) buf[i] = 0;
    if (len) buf[0] = report_id;
    return len;
}

void usb_hid_on_output(const uint8_t *buf, uint16_t len, bool via_control, uint8_t control_report_id) {
    if (usb_mode_active() == USB_MODE_DUALSENSE_EDGE) ds5_on_output(buf, len, via_control, control_report_id);
    else procon_on_output(buf, len, via_control, control_report_id);
}
