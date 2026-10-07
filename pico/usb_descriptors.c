// USB descriptors: the device presents itself like a wired Nintendo Switch Pro
// Controller (057E:2009). Interface 0 is the controller's HID interface. When
// WebUSB is enabled, interface 1 is a vendor interface for the configuration
// page and the device advertises USB 2.1 + a BOS descriptor (WebUSB landing
// page, Microsoft OS 2.0 descriptors for automatic WinUSB on Windows). With
// WebUSB disabled the descriptors match a genuine controller.

#include <assert.h>
#include <string.h>

#include "tusb.h"
#include "class/hid/hid.h"

#include "settings.h"
#include "usb_hid.h"
#include "webusb.h"

#define PRO_VID 0x057E
#define PRO_PID 0x2009

static tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = PRO_VID,
    .idProduct = PRO_PID,
    .bcdDevice = 0x0200,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    // A distinct bcdDevice keeps Windows from reusing a cached "no MS OS
    // descriptor" answer from a genuine Pro Controller.
    s_device.bcdUSB = g_settings.webusb_enabled ? 0x0210 : 0x0200;
    s_device.bcdDevice = g_settings.webusb_enabled ? 0x0201 : 0x0200;
    return (uint8_t const *)&s_device;
}

#define BOS_LEN (TUD_BOS_DESC_LEN + TUD_BOS_WEBUSB_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
static const uint8_t s_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_LEN, 2),
    TUD_BOS_WEBUSB_DESCRIPTOR(WEBUSB_VENDOR_REQUEST_URL, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(0xB2, WEBUSB_VENDOR_REQUEST_MS),
};

uint8_t const *tud_descriptor_bos_cb(void) {
    return g_settings.webusb_enabled ? s_bos : NULL;
}

// ---------------------------------------------------------------------------
// Configuration descriptor
// ---------------------------------------------------------------------------
#define EP_IN  0x81
#define EP_OUT 0x01
#define EP_VENDOR_IN  0x82
#define EP_VENDOR_OUT 0x02
#define CONFIG_LEN_HID (TUD_CONFIG_DESC_LEN + 9 + 9 + 7 + 7)
#define CONFIG_LEN (CONFIG_LEN_HID + TUD_VENDOR_DESC_LEN)

static uint8_t s_config[CONFIG_LEN];

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    uint16_t rlen;
    usb_hid_report_descriptor(&rlen);
    // Poll every 1 ms: input reports are paced by usb_report_interval_ms,
    // and replies to a host's burst of init commands go out without queueing
    // behind an 8 ms poll.
    const uint8_t interval = 1;
    bool webusb = g_settings.webusb_enabled;
    uint16_t total = webusb ? CONFIG_LEN : CONFIG_LEN_HID;
    const uint8_t desc[] = {
        // Configuration: 1 or 2 interfaces, bus powered, remote wakeup, 500 mA
        9, TUSB_DESC_CONFIGURATION, U16_TO_U8S_LE(total), (uint8_t)(webusb ? 2 : 1), 1, 0, 0xA0, 0xFA,
        // Interface 0: HID, no boot protocol
        9, TUSB_DESC_INTERFACE, 0, 0, 2, TUSB_CLASS_HID, 0, 0, 0,
        // HID descriptor
        9, HID_DESC_TYPE_HID, U16_TO_U8S_LE(0x0111), 0, 1, HID_DESC_TYPE_REPORT, U16_TO_U8S_LE(rlen),
        // Endpoints
        7, TUSB_DESC_ENDPOINT, EP_IN, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(USB_HID_EP_SIZE), interval,
        7, TUSB_DESC_ENDPOINT, EP_OUT, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(USB_HID_EP_SIZE), interval,
        // Interface 1: WebUSB configuration (vendor class, bulk)
        TUD_VENDOR_DESCRIPTOR(1, 4, EP_VENDOR_OUT, EP_VENDOR_IN, 64),
    };
    static_assert(sizeof desc == CONFIG_LEN, "config descriptor length");
    memcpy(s_config, desc, sizeof desc);
    return s_config;
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------
static const char *const s_strings[] = {
    NULL,                   // 0: language (handled below)
    "Nintendo Co., Ltd.",   // 1
    "Pro Controller",       // 2
    "000000000001",         // 3
    "Switch2-Pico Config",  // 4
};

static uint16_t s_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;
    if (index == 0) {
        s_str[1] = 0x0409;
        chr_count = 1;
    } else {
        if (index >= sizeof s_strings / sizeof s_strings[0]) return NULL;
        const char *str = s_strings[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) s_str[1 + i] = str[i];
    }
    s_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return s_str;
}
