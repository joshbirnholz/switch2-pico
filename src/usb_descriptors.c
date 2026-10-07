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
// HID report descriptor of a genuine Pro Controller, optionally extended with
// the 0x31 (NFC/IR MCU) input report so hosts can receive the 361 byte
// report over USB. A genuine controller only declares 0x30, 0x21 and 0x81.
// ---------------------------------------------------------------------------
static const uint8_t PRO_REPORT_DESC_HEAD[] = {
    0x05, 0x01,                   // Usage Page (Generic Desktop)
    0x15, 0x00,                   // Logical Minimum (0)
    0x09, 0x04,                   // Usage (Joystick)
    0xA1, 0x01,                   // Collection (Application)
    0x85, 0x30,                   //   Report ID (0x30)
    0x05, 0x01,                   //   Usage Page (Generic Desktop)
    0x05, 0x09,                   //   Usage Page (Button)
    0x19, 0x01, 0x29, 0x0A,       //   Usage Minimum (1), Maximum (10)
    0x15, 0x00, 0x25, 0x01,       //   Logical 0..1
    0x75, 0x01, 0x95, 0x0A,       //   10 x 1 bit
    0x55, 0x00, 0x65, 0x00,       //   Unit Exponent 0, Unit none
    0x81, 0x02,                   //   Input (Data, Var, Abs)
    0x05, 0x09,                   //   Usage Page (Button)
    0x19, 0x0B, 0x29, 0x0E,       //   Usage Minimum (11), Maximum (14)
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x04,
    0x81, 0x02,                   //   Input (Data, Var, Abs)
    0x75, 0x01, 0x95, 0x02,
    0x81, 0x03,                   //   Input (Const)
    0x0B, 0x01, 0x00, 0x01, 0x00, //   Usage (Generic Desktop: Pointer)
    0xA1, 0x00,                   //   Collection (Physical)
    0x0B, 0x30, 0x00, 0x01, 0x00, //     Usage (X)
    0x0B, 0x31, 0x00, 0x01, 0x00, //     Usage (Y)
    0x0B, 0x32, 0x00, 0x01, 0x00, //     Usage (Z)
    0x0B, 0x35, 0x00, 0x01, 0x00, //     Usage (Rz)
    0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00, // Logical 0..65535
    0x75, 0x10, 0x95, 0x04,       //     4 x 16 bit
    0x81, 0x02,                   //     Input (Data, Var, Abs)
    0xC0,                         //   End Collection
    0x0B, 0x39, 0x00, 0x01, 0x00, //   Usage (Hat switch)
    0x15, 0x00, 0x25, 0x07,       //   Logical 0..7
    0x35, 0x00, 0x46, 0x3B, 0x01, //   Physical 0..315
    0x65, 0x14,                   //   Unit (degrees)
    0x75, 0x04, 0x95, 0x01,
    0x81, 0x42,                   //   Input (Data, Var, Abs, Null)
    0x05, 0x09,                   //   Usage Page (Button)
    0x19, 0x0F, 0x29, 0x12,       //   Usage Minimum (15), Maximum (18)
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x04,
    0x81, 0x02,
    0x75, 0x08, 0x95, 0x34,
    0x81, 0x03,                   //   52 bytes padding
    0x06, 0x00, 0xFF,             //   Usage Page (Vendor 0xFF00)
    0x85, 0x21, 0x09, 0x01, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03, // 0x21 in, 63 bytes
    0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03, // 0x81 in, 63 bytes
    0x85, 0x01, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, // 0x01 out
    0x85, 0x10, 0x09, 0x04, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, // 0x10 out
    0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, // 0x80 out
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, // 0x82 out
};

static const uint8_t PRO_REPORT_DESC_NFC[] = {
    0x85, 0x31, 0x09, 0x07, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x03, // 0x31 in, 361 bytes
    0x85, 0x11, 0x09, 0x08, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,       // 0x11 out (MCU request)
};

static const uint8_t PRO_REPORT_DESC_TAIL[] = {
    0xC0, // End Collection
};

static uint8_t s_report_desc[sizeof PRO_REPORT_DESC_HEAD + sizeof PRO_REPORT_DESC_NFC + sizeof PRO_REPORT_DESC_TAIL];
static uint16_t s_report_desc_len;

static void build_report_desc(void) {
    uint16_t n = 0;
    memcpy(s_report_desc + n, PRO_REPORT_DESC_HEAD, sizeof PRO_REPORT_DESC_HEAD);
    n += sizeof PRO_REPORT_DESC_HEAD;
    if (g_settings.nfc_enabled && g_settings.nfc_report_in_descriptor) {
        memcpy(s_report_desc + n, PRO_REPORT_DESC_NFC, sizeof PRO_REPORT_DESC_NFC);
        n += sizeof PRO_REPORT_DESC_NFC;
    }
    memcpy(s_report_desc + n, PRO_REPORT_DESC_TAIL, sizeof PRO_REPORT_DESC_TAIL);
    n += sizeof PRO_REPORT_DESC_TAIL;
    s_report_desc_len = n;
}

const uint8_t *usb_hid_report_descriptor(uint16_t *len) {
    build_report_desc();   // cheap; settings may have changed before re-enumeration
    *len = s_report_desc_len;
    return s_report_desc;
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
    uint8_t interval = g_settings.usb_report_interval_ms ? g_settings.usb_report_interval_ms : 8;
    if (interval > 8) interval = 8;
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
