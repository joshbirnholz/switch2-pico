#include "usb_hid.h"

#include <string.h>

#include "tusb.h"
#include "class/hid/hid.h"
#include "device/usbd_pvt.h"

typedef struct {
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t idle_rate;
    uint8_t protocol;
    tusb_hid_descriptor_hid_t const *hid_desc;
} pro_hid_t;

static pro_hid_t s_hid;

CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_in_buf[USB_HID_MAX_IN_REPORT];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_out_buf[USB_HID_EP_SIZE];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_ctrl_buf[USB_HID_EP_SIZE];

// Single producer (USB stack) / single consumer (main loop) queue of output reports.
#define OUT_QUEUE_LEN 8
typedef struct {
    uint8_t data[USB_HID_EP_SIZE];
    uint8_t len;
    bool via_control;
    uint8_t report_id;
} out_report_t;
static out_report_t s_outq[OUT_QUEUE_LEN];
static volatile uint8_t s_outq_head, s_outq_tail;

static void out_enqueue(const uint8_t *buf, uint16_t len, bool via_control, uint8_t report_id) {
    uint8_t t = s_outq_tail;
    uint8_t next = (uint8_t)((t + 1) % OUT_QUEUE_LEN);
    if (next == s_outq_head) return;   // full: drop (the host resends rumble/subcommands)
    if (len > USB_HID_EP_SIZE) len = USB_HID_EP_SIZE;
    memcpy(s_outq[t].data, buf, len);
    s_outq[t].len = (uint8_t)len;
    s_outq[t].via_control = via_control;
    s_outq[t].report_id = report_id;
    __sync_synchronize();
    s_outq_tail = next;
}

void usb_hid_task(void) {
    while (s_outq_head != s_outq_tail) {
        out_report_t *r = &s_outq[s_outq_head];
        __sync_synchronize();
        usb_hid_on_output(r->data, r->len, r->via_control, r->report_id);
        s_outq_head = (uint8_t)((s_outq_head + 1) % OUT_QUEUE_LEN);
    }
}

bool usb_hid_mounted(void) {
    return tud_mounted() && s_hid.ep_in != 0;
}

bool usb_hid_ready(void) {
    return usb_hid_mounted() && !tud_suspended() && usbd_edpt_ready(0, s_hid.ep_in);
}

bool usb_hid_suspended(void) {
    return tud_suspended();
}

bool usb_hid_ep_ready(void) {
    return s_hid.ep_in != 0 && usbd_edpt_ready(0, s_hid.ep_in);
}

bool usb_hid_send(uint8_t id, const uint8_t *data, uint16_t len) {
    if (!usb_hid_ready()) return false;
    if (len + 1u > sizeof s_in_buf) return false;
    if (!usbd_edpt_claim(0, s_hid.ep_in)) return false;
    s_in_buf[0] = id;
    memcpy(s_in_buf + 1, data, len);
    bool ok = usbd_edpt_xfer(0, s_hid.ep_in, s_in_buf, (uint16_t)(len + 1));
    if (!ok) usbd_edpt_release(0, s_hid.ep_in);
    return ok;
}

static void prohid_init(void) {
    memset(&s_hid, 0, sizeof s_hid);
}

static bool prohid_deinit(void) {
    return true;
}

static void prohid_reset(uint8_t rhport) {
    (void)rhport;
    memset(&s_hid, 0, sizeof s_hid);
}

static uint16_t prohid_open(uint8_t rhport, tusb_desc_interface_t const *desc_itf, uint16_t max_len) {
    // HID, or the Xbox 360's vendor-class XInput interface (same shape: one
    // class descriptor of type 0x21, then an interrupt IN/OUT pair).
    // (A boot mouse is the mouse driver's, below.)
    bool hid = desc_itf->bInterfaceClass == TUSB_CLASS_HID && desc_itf->bInterfaceProtocol != HID_ITF_PROTOCOL_MOUSE;
    bool xinput = desc_itf->bInterfaceClass == 0xFF && desc_itf->bInterfaceSubClass == 0x5D;
    TU_VERIFY(hid || xinput, 0);
    uint8_t const *p_desc = tu_desc_next(desc_itf);
    TU_ASSERT(tu_desc_type(p_desc) == HID_DESC_TYPE_HID, 0);
    uint16_t const drv_len = (uint16_t)(sizeof(tusb_desc_interface_t) + tu_desc_len(p_desc) +
                                        desc_itf->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
    TU_ASSERT(max_len >= drv_len, 0);
    s_hid.hid_desc = hid ? (tusb_hid_descriptor_hid_t const *)p_desc : NULL;
    p_desc = tu_desc_next(p_desc);
    TU_ASSERT(usbd_open_edpt_pair(rhport, p_desc, desc_itf->bNumEndpoints, TUSB_XFER_INTERRUPT,
                                  &s_hid.ep_out, &s_hid.ep_in), 0);
    s_hid.itf_num = desc_itf->bInterfaceNumber;
    s_hid.protocol = HID_PROTOCOL_REPORT;
    if (s_hid.ep_out) {
        TU_ASSERT(usbd_edpt_xfer(rhport, s_hid.ep_out, s_out_buf, sizeof s_out_buf), drv_len);
    }
    return drv_len;
}

static bool prohid_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    TU_VERIFY(request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE);
    TU_VERIFY(tu_u16_low(request->wIndex) == s_hid.itf_num);

    if (!s_hid.hid_desc && request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR) {
        // XInput: hosts may probe with vendor requests; answer IN requests
        // with zeros and accept OUT data, rather than stalling.
        if (stage != CONTROL_STAGE_SETUP) return true;
        uint16_t n = tu_min16(request->wLength, sizeof s_ctrl_buf);
        memset(s_ctrl_buf, 0, sizeof s_ctrl_buf);
        if (n == 0) return tud_control_status(rhport, request);
        return tud_control_xfer(rhport, request, s_ctrl_buf, n);
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        uint8_t const desc_type = tu_u16_high(request->wValue);
        if (request->bRequest == TUSB_REQ_GET_DESCRIPTOR && desc_type == HID_DESC_TYPE_HID) {
            TU_VERIFY(s_hid.hid_desc);
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_hid.hid_desc, s_hid.hid_desc->bLength);
        }
        if (request->bRequest == TUSB_REQ_GET_DESCRIPTOR && desc_type == HID_DESC_TYPE_REPORT) {
            uint16_t len;
            const uint8_t *desc = usb_hid_report_descriptor(&len);
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc, len);
        }
        return false;
    }

    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) return false;

    switch (request->bRequest) {
    case HID_REQ_CONTROL_GET_REPORT:
        if (stage == CONTROL_STAGE_SETUP) {
            uint16_t n = tu_min16(request->wLength, sizeof s_ctrl_buf);
            memset(s_ctrl_buf, 0, sizeof s_ctrl_buf);
            n = usb_hid_get_feature(tu_u16_low(request->wValue), s_ctrl_buf, n);
            return tud_control_xfer(rhport, request, s_ctrl_buf, n);
        }
        return true;
    case HID_REQ_CONTROL_SET_REPORT:
        if (stage == CONTROL_STAGE_SETUP) {
            TU_VERIFY(request->wLength <= sizeof s_ctrl_buf);
            return tud_control_xfer(rhport, request, s_ctrl_buf, request->wLength);
        }
        if (stage == CONTROL_STAGE_ACK) {
            out_enqueue(s_ctrl_buf, request->wLength, true, tu_u16_low(request->wValue));
        }
        return true;
    case HID_REQ_CONTROL_SET_IDLE:
        if (stage == CONTROL_STAGE_SETUP) {
            s_hid.idle_rate = tu_u16_high(request->wValue);
            return tud_control_status(rhport, request);
        }
        return true;
    case HID_REQ_CONTROL_GET_IDLE:
        if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, request, &s_hid.idle_rate, 1);
        return true;
    case HID_REQ_CONTROL_GET_PROTOCOL:
        if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, request, &s_hid.protocol, 1);
        return true;
    case HID_REQ_CONTROL_SET_PROTOCOL:
        if (stage == CONTROL_STAGE_SETUP) return tud_control_status(rhport, request);
        if (stage == CONTROL_STAGE_ACK) s_hid.protocol = (uint8_t)request->wValue;
        return true;
    default:
        return false;
    }
}

static bool prohid_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    if (ep_addr == s_hid.ep_out) {
        if (result == XFER_RESULT_SUCCESS && xferred_bytes > 0) {
            out_enqueue(s_out_buf, (uint16_t)xferred_bytes, false, 0);
        }
        TU_ASSERT(usbd_edpt_xfer(rhport, s_hid.ep_out, s_out_buf, sizeof s_out_buf));
    }
    return true;
}


// ---------------------------------------------------------------------------
// USB mouse: a plain boot-compatible mouse (5 buttons, x, y, wheel).
// ---------------------------------------------------------------------------
static const uint8_t MOUSE_REPORT_DESC[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01,          // Generic Desktop, Mouse, Application
    0x09, 0x01, 0xA1, 0x00,                      //   Pointer, Physical
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05,          //     Buttons 1..5
    0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,          //     padding
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,   // X, Y, Wheel
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
    0xC0, 0xC0,
};

typedef struct {
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t idle_rate;
    uint8_t protocol;
} mouse_hid_t;

static mouse_hid_t s_mouse;
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_mouse_buf[USB_MOUSE_EP_SIZE];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_mouse_ctrl[USB_MOUSE_EP_SIZE];

uint16_t usb_mouse_interface_desc(uint8_t *buf, uint16_t cap, uint8_t itf, uint8_t ep_in) {
    const uint16_t rlen = sizeof MOUSE_REPORT_DESC;
    const uint8_t desc[] = {
        9, 0x04, itf, 0, 1, 0x03, 0x01, 0x02, 0,                                       // HID, boot, mouse
        9, 0x21, 0x11, 0x01, 0, 1, 0x22, (uint8_t)rlen, (uint8_t)(rlen >> 8),
        7, 0x05, ep_in, 0x03, USB_MOUSE_EP_SIZE, 0, 1,
    };
    if (cap < sizeof desc) return 0;
    memcpy(buf, desc, sizeof desc);
    return sizeof desc;
}

bool usb_mouse_ready(void) {
    return tud_mounted() && !tud_suspended() && s_mouse.ep_in != 0 && usbd_edpt_ready(0, s_mouse.ep_in);
}

bool usb_mouse_send(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    if (!usb_mouse_ready()) return false;
    if (!usbd_edpt_claim(0, s_mouse.ep_in)) return false;
    s_mouse_buf[0] = buttons;
    s_mouse_buf[1] = (uint8_t)dx;
    s_mouse_buf[2] = (uint8_t)dy;
    s_mouse_buf[3] = (uint8_t)wheel;
    // Boot protocol: buttons, x, y only.
    uint16_t n = s_mouse.protocol == HID_PROTOCOL_BOOT ? 3 : 4;
    bool ok = usbd_edpt_xfer(0, s_mouse.ep_in, s_mouse_buf, n);
    if (!ok) usbd_edpt_release(0, s_mouse.ep_in);
    return ok;
}

static void mouse_init(void) {
    memset(&s_mouse, 0, sizeof s_mouse);
}

static bool mouse_deinit(void) {
    return true;
}

static void mouse_reset(uint8_t rhport) {
    (void)rhport;
    memset(&s_mouse, 0, sizeof s_mouse);
}

static uint16_t mouse_open(uint8_t rhport, tusb_desc_interface_t const *desc_itf, uint16_t max_len) {
    TU_VERIFY(desc_itf->bInterfaceClass == TUSB_CLASS_HID && desc_itf->bInterfaceProtocol == HID_ITF_PROTOCOL_MOUSE, 0);
    uint8_t const *p_desc = tu_desc_next(desc_itf);
    TU_ASSERT(tu_desc_type(p_desc) == HID_DESC_TYPE_HID, 0);
    uint16_t const drv_len = (uint16_t)(sizeof(tusb_desc_interface_t) + tu_desc_len(p_desc) +
                                        desc_itf->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
    TU_ASSERT(max_len >= drv_len, 0);
    p_desc = tu_desc_next(p_desc);
    uint8_t ep_out = 0;
    TU_ASSERT(usbd_open_edpt_pair(rhport, p_desc, desc_itf->bNumEndpoints, TUSB_XFER_INTERRUPT, &ep_out,
                                  &s_mouse.ep_in), 0);
    s_mouse.itf_num = desc_itf->bInterfaceNumber;
    s_mouse.protocol = HID_PROTOCOL_REPORT;
    return drv_len;
}

static bool mouse_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    TU_VERIFY(request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE);
    TU_VERIFY(tu_u16_low(request->wIndex) == s_mouse.itf_num);
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        uint8_t const desc_type = tu_u16_high(request->wValue);
        if (request->bRequest == TUSB_REQ_GET_DESCRIPTOR && desc_type == HID_DESC_TYPE_REPORT) {
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)MOUSE_REPORT_DESC, sizeof MOUSE_REPORT_DESC);
        }
        return false;
    }
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) return false;
    switch (request->bRequest) {
    case HID_REQ_CONTROL_GET_REPORT:
        if (stage == CONTROL_STAGE_SETUP) {
            memset(s_mouse_ctrl, 0, sizeof s_mouse_ctrl);
            return tud_control_xfer(rhport, request, s_mouse_ctrl, tu_min16(request->wLength, 4));
        }
        return true;
    case HID_REQ_CONTROL_SET_IDLE:
        if (stage == CONTROL_STAGE_SETUP) {
            s_mouse.idle_rate = tu_u16_high(request->wValue);
            return tud_control_status(rhport, request);
        }
        return true;
    case HID_REQ_CONTROL_GET_IDLE:
        if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, request, &s_mouse.idle_rate, 1);
        return true;
    case HID_REQ_CONTROL_GET_PROTOCOL:
        if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, request, &s_mouse.protocol, 1);
        return true;
    case HID_REQ_CONTROL_SET_PROTOCOL:
        if (stage == CONTROL_STAGE_SETUP) return tud_control_status(rhport, request);
        if (stage == CONTROL_STAGE_ACK) s_mouse.protocol = (uint8_t)request->wValue;
        return true;
    default:
        return false;
    }
}

static bool mouse_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    (void)rhport;
    (void)ep_addr;
    (void)result;
    (void)xferred_bytes;
    return true;
}

static usbd_class_driver_t const s_drivers[] = {
    {
#if CFG_TUSB_DEBUG >= 2
        .name = "PROHID",
#else
        .name = NULL,
#endif
        .init = prohid_init,
        .deinit = prohid_deinit,
        .reset = prohid_reset,
        .open = prohid_open,
        .control_xfer_cb = prohid_control_xfer_cb,
        .xfer_cb = prohid_xfer_cb,
        .sof = NULL,
    },
    {
#if CFG_TUSB_DEBUG >= 2
        .name = "MOUSE",
#else
        .name = NULL,
#endif
        .init = mouse_init,
        .deinit = mouse_deinit,
        .reset = mouse_reset,
        .open = mouse_open,
        .control_xfer_cb = mouse_control_xfer_cb,
        .xfer_cb = mouse_xfer_cb,
        .sof = NULL,
    },
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = sizeof s_drivers / sizeof s_drivers[0];
    return s_drivers;
}
