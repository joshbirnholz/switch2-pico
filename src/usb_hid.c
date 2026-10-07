#include "usb_hid.h"

#include <string.h>

#include "tusb.h"
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

bool usb_hid_mounted(void) {
    return tud_mounted() && s_hid.ep_in != 0;
}

bool usb_hid_ready(void) {
    return usb_hid_mounted() && !tud_suspended() && usbd_edpt_ready(0, s_hid.ep_in);
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
    TU_VERIFY(desc_itf->bInterfaceClass == TUSB_CLASS_HID, 0);
    uint16_t const drv_len = (uint16_t)(sizeof(tusb_desc_interface_t) + sizeof(tusb_hid_descriptor_hid_t) +
                                        desc_itf->bNumEndpoints * sizeof(tusb_desc_endpoint_t));
    TU_ASSERT(max_len >= drv_len, 0);

    uint8_t const *p_desc = tu_desc_next(desc_itf);
    TU_ASSERT(tu_desc_type(p_desc) == HID_DESC_TYPE_HID, 0);
    s_hid.hid_desc = (tusb_hid_descriptor_hid_t const *)p_desc;
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
            // Nothing meaningful to return; answer with an empty report of the
            // requested id so hosts that probe don't stall.
            uint16_t n = tu_min16(request->wLength, sizeof s_ctrl_buf);
            memset(s_ctrl_buf, 0, n);
            if (n) s_ctrl_buf[0] = tu_u16_low(request->wValue);
            return tud_control_xfer(rhport, request, s_ctrl_buf, n);
        }
        return true;
    case HID_REQ_CONTROL_SET_REPORT:
        if (stage == CONTROL_STAGE_SETUP) {
            TU_VERIFY(request->wLength <= sizeof s_ctrl_buf);
            return tud_control_xfer(rhport, request, s_ctrl_buf, request->wLength);
        }
        if (stage == CONTROL_STAGE_ACK) {
            usb_hid_on_output(s_ctrl_buf, request->wLength, true, tu_u16_low(request->wValue));
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
            usb_hid_on_output(s_out_buf, (uint16_t)xferred_bytes, false, 0);
        }
        TU_ASSERT(usbd_edpt_xfer(rhport, s_hid.ep_out, s_out_buf, sizeof s_out_buf));
    }
    return true;
}

static usbd_class_driver_t const s_driver = {
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
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &s_driver;
}
