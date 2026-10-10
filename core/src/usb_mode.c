#include "usb_mode.h"

#include <string.h>

#include "ds5.h"
#include "gc_adapter.h"
#include "sinput.h"
#include "usb_hid.h"
#include "x360.h"

static const usb_identity_t IDENTITIES[USB_MODE_COUNT] = {
    [USB_MODE_SWITCH_PRO] = {0x057E, 0x2009, 0x0200, 0x00, "Nintendo Co., Ltd.", "Pro Controller"},
    [USB_MODE_DUALSENSE_EDGE] = {0x054C, 0x0DF2, 0x0100, 0x00, "Sony Interactive Entertainment",
                                 "DualSense Edge Wireless Controller"},
    [USB_MODE_DUALSENSE] = {0x054C, 0x0CE6, 0x0100, 0x00, "Sony Interactive Entertainment",
                            "DualSense Wireless Controller"},
    [USB_MODE_XBOX360] = {0x045E, 0x028E, 0x0114, 0xFF, "Microsoft", "Controller"},
    [USB_MODE_GC_ADAPTER] = {0x057E, 0x0337, 0x0100, 0x00, "Nintendo", "WUP-028"},
    // Hand Held Legend's generic SInput ID (SDL picks the protocol by it).
    [USB_MODE_SINPUT] = {0x2E8A, 0x10C6, 0x0100, 0x00, "Switch2-Pico", "Switch2-Pico SInput"},
};

// pid.codes' test ID (1209:0001): a plain device, nothing binds a driver to it.
static const usb_identity_t CONFIG_ONLY = {0x1209, 0x0001, 0x0100, 0x00, "Switch2-Pico", "Switch2-Pico (no controller)"};
static bool s_config_only;

void usb_mode_set_config_only(bool on) {
    s_config_only = on;
}

bool usb_mode_config_only(void) {
    return s_config_only;
}

static const char *const NAMES[USB_MODE_COUNT] = {
    [USB_MODE_SWITCH_PRO] = "switch_pro",
    [USB_MODE_DUALSENSE_EDGE] = "dualsense_edge",
    [USB_MODE_DUALSENSE] = "dualsense",
    [USB_MODE_XBOX360] = "xbox360",
    [USB_MODE_GC_ADAPTER] = "gc_adapter",
    [USB_MODE_SINPUT] = "sinput",
};

usb_mode_t usb_mode_active(void) {
    // Latched at first use (boot): descriptors must not change under the host.
    static int mode = -1;
    if (mode < 0) {
        uint8_t m = settings_boot_usb_mode(&g_settings);
        mode = m < USB_MODE_COUNT ? m : USB_MODE_SWITCH_PRO;
    }
    return (usb_mode_t)mode;
}

bool usb_mode_has_mouse(void) {
    static int mouse = -1;
    if (mouse < 0) mouse = settings_boot_mouse(&g_settings) ? 1 : 0;
    return mouse == 1;
}

const usb_identity_t *usb_mode_identity(void) {
    if (s_config_only) return &CONFIG_ONLY;
    return &IDENTITIES[usb_mode_active()];
}

const char *usb_mode_name(usb_mode_t m) {
    return m < USB_MODE_COUNT ? NAMES[m] : "?";
}

bool usb_mode_is_hid(void) {
    return usb_mode_active() != USB_MODE_XBOX360;
}

// ---------------------------------------------------------------------------
// Output names of the generic (non-Switch) modes; NULL = not in this mode.
// ---------------------------------------------------------------------------
static bool is_ds(usb_mode_t m) {
    return m == USB_MODE_DUALSENSE || m == USB_MODE_DUALSENSE_EDGE;
}

// GameCube adapter: the outputs a GameCube controller has (gc_adapter.c
// packs them: South A, West B, East X, North Y, R1 Z, L2 / R2 L / R).
static const char *gc_output_name(gp_out_t g) {
    switch (g) {
    case GP_NONE: return "None";
    case GP_SOUTH: return "A";
    case GP_WEST: return "B";
    case GP_EAST: return "X";
    case GP_NORTH: return "Y";
    case GP_R1: return "Z";
    case GP_L2: return "L";
    case GP_R2: return "R";
    case GP_START: return "Start";
    case GP_UP: return "Up";
    case GP_DOWN: return "Down";
    case GP_LEFT: return "Left";
    case GP_RIGHT: return "Right";
    case GP_DECKY_QAM: return "Quick Access Menu (Decky)";
    default: return NULL;
    }
}

// SInput: Nintendo labels (by position: South B, East A, West Y, North X).
// Steam shows the second paddle pair and the extra button with the Switch 2
// Pro Controller's GL, GR and C symbols; the first pair (L4 / R4) has none
// (SDL takes the second pair only together with the first). No Steam
// shortcuts: the Decky Quick Access Menu output and Capture do their jobs.
static const char *sinput_output_name(gp_out_t g) {
    switch (g) {
    case GP_NONE: return "None";
    case GP_SOUTH: return "B";
    case GP_EAST: return "A";
    case GP_WEST: return "Y";
    case GP_NORTH: return "X";
    case GP_L1: return "L";
    case GP_R1: return "R";
    case GP_L2: return "ZL";
    case GP_R2: return "ZR";
    case GP_SELECT: return "Minus";
    case GP_START: return "Plus";
    case GP_L3: return "LStick";
    case GP_R3: return "RStick";
    case GP_GUIDE: return "Home";
    case GP_UP: return "Up";
    case GP_DOWN: return "Down";
    case GP_LEFT: return "Left";
    case GP_RIGHT: return "Right";
    case GP_MIC: return "Capture";
    case GP_PADDLE_L: return "L4 (extra button, no symbol in Steam)";
    case GP_PADDLE_R: return "R4 (extra button, no symbol in Steam)";
    case GP_FN_L: return "GL";
    case GP_FN_R: return "GR";
    case GP_MISC: return "C";
    case GP_DECKY_QAM: return "Quick Access Menu (Decky)";
    default: return NULL;
    }
}

const char *usb_mode_output_name(usb_mode_t m, gp_out_t g) {
    if (m == USB_MODE_GC_ADAPTER) return gc_output_name(g);
    if (m == USB_MODE_SINPUT) return sinput_output_name(g);
    bool ds = is_ds(m), x = m == USB_MODE_XBOX360;
    switch (g) {
    case GP_NONE: return "None";
    case GP_SOUTH: return ds ? "Cross" : "A";
    case GP_EAST: return ds ? "Circle" : "B";
    case GP_WEST: return ds ? "Square" : "X";
    case GP_NORTH: return ds ? "Triangle" : "Y";
    case GP_L1: return ds ? "L1" : "LB";
    case GP_R1: return ds ? "R1" : "RB";
    case GP_L2: return ds ? "L2" : "LT";
    case GP_R2: return ds ? "R2" : "RT";
    case GP_SELECT: return ds ? "Create" : "Back";
    case GP_START: return ds ? "Options" : "Start";
    case GP_L3: return ds ? "L3" : "LS";
    case GP_R3: return ds ? "R3" : "RS";
    case GP_GUIDE: return ds ? "PS" : "Guide";
    case GP_UP: return "Up";
    case GP_DOWN: return "Down";
    case GP_LEFT: return "Left";
    case GP_RIGHT: return "Right";
    case GP_TOUCHPAD: return ds ? "Touchpad click (center)" : NULL;
    case GP_TP_LEFT: return ds ? "Touchpad click (left)" : NULL;
    case GP_TP_RIGHT: return ds ? "Touchpad click (right)" : NULL;
    case GP_MIC: return ds ? "Mic (mute)" : NULL;
    case GP_PADDLE_L: return m == USB_MODE_DUALSENSE_EDGE ? "Left paddle" : NULL;
    case GP_PADDLE_R: return m == USB_MODE_DUALSENSE_EDGE ? "Right paddle" : NULL;
    case GP_FN_L: return m == USB_MODE_DUALSENSE_EDGE ? "Left Fn" : NULL;
    case GP_FN_R: return m == USB_MODE_DUALSENSE_EDGE ? "Right Fn" : NULL;
    case GP_MACRO_QAM: return ds ? "PS+Cross (Steam quick access)" : x ? "Guide+A (Steam quick access)" : NULL;
    case GP_MACRO_SHOT: return ds ? "PS+R1 (Steam screenshot)" : x ? "Guide+RB (Steam screenshot)" : NULL;
    case GP_DECKY_QAM: return "Quick Access Menu (Decky)";
    default: return NULL;
    }
}

void usb_mode_get_status(procon_status_t *out) {
    switch (usb_mode_active()) {
    case USB_MODE_DUALSENSE_EDGE:
    case USB_MODE_DUALSENSE: ds5_get_status(out); break;
    case USB_MODE_XBOX360: x360_get_status(out); break;
    case USB_MODE_GC_ADAPTER: gc_adapter_get_status(out); break;
    case USB_MODE_SINPUT: sinput_get_status(out); break;
    default: procon_get_status(out); break;
    }
}

// ---------------------------------------------------------------------------
// USB plumbing
// ---------------------------------------------------------------------------
uint16_t usb_mode_interface_desc(uint8_t *buf, uint16_t cap, uint8_t itf, uint8_t ep_in, uint8_t ep_out) {
    if (usb_mode_active() == USB_MODE_XBOX360) {
        if (cap < X360_ITF_DESC_LEN) return 0;
        x360_interface_desc(buf, itf, ep_in, ep_out);
        return X360_ITF_DESC_LEN;
    }
    uint16_t rlen;
    usb_hid_report_descriptor(&rlen);
    // Poll every 1 ms: input reports are paced by usb_report_interval_ms, and
    // replies to a host's burst of init commands don't queue behind slow polls.
    const uint8_t desc[] = {
        9, 0x04, itf, 0, 2, 0x03, 0, 0, 0,                                         // interface: HID
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, (uint8_t)rlen, (uint8_t)(rlen >> 8),      // HID 1.11
        7, 0x05, ep_in, 0x03, USB_HID_EP_SIZE, 0, 1,
        7, 0x05, ep_out, 0x03, USB_HID_EP_SIZE, 0, 1,
    };
    if (cap < sizeof desc) return 0;
    memcpy(buf, desc, sizeof desc);
    return sizeof desc;
}

uint16_t usb_mode_interface_desc_len(void) {
    return usb_mode_active() == USB_MODE_XBOX360 ? X360_ITF_DESC_LEN : 9 + 9 + 7 + 7;
}

const uint8_t *usb_hid_report_descriptor(uint16_t *len) {
    if (is_ds(usb_mode_active())) return ds5_report_descriptor(len);
    if (usb_mode_active() == USB_MODE_GC_ADAPTER) return gc_adapter_report_descriptor(len);
    if (usb_mode_active() == USB_MODE_SINPUT) return sinput_report_descriptor(len);
    return procon_report_descriptor(len);
}

uint16_t usb_hid_get_feature(uint8_t report_id, uint8_t *buf, uint16_t len) {
    if (is_ds(usb_mode_active())) return ds5_get_feature(report_id, buf, len);
    // Switch Pro: nothing meaningful; an empty report of the requested ID.
    for (uint16_t i = 0; i < len; i++) buf[i] = 0;
    if (len) buf[0] = report_id;
    return len;
}

void usb_hid_on_output(const uint8_t *buf, uint16_t len, bool via_control, uint8_t control_report_id) {
    switch (usb_mode_active()) {
    case USB_MODE_DUALSENSE_EDGE:
    case USB_MODE_DUALSENSE: ds5_on_output(buf, len, via_control, control_report_id); break;
    case USB_MODE_XBOX360:
        if (!via_control) x360_on_output(buf, len);
        break;
    case USB_MODE_GC_ADAPTER: gc_adapter_on_output(buf, len); break;
    case USB_MODE_SINPUT: sinput_on_output(buf, len, via_control, control_report_id); break;
    default: procon_on_output(buf, len, via_control, control_report_id); break;
    }
}
