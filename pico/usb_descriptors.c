// USB descriptors: the device presents itself like a wired Nintendo Switch Pro
// Controller (057E:2009) or, in DualSense Edge mode, like a DualSense Edge
// (054C:0DF2); see core/src/usb_mode.c. Interface 0 is the controller's HID interface. When
// WebUSB is enabled, interface 1 is a vendor interface for the configuration
// page and the device advertises USB 2.1 + a BOS descriptor (WebUSB landing
// page, Microsoft OS 2.0 descriptors for automatic WinUSB on Windows). With
// WebUSB disabled the descriptors match a genuine controller. A profile that
// uses the Joy-Con 2 mouse adds a boot mouse interface last.

#include <assert.h>
#include <string.h>

#include "tusb.h"
#include "class/hid/hid.h"

#include "settings.h"
#include "usb_hid.h"
#include "usb_mode.h"
#include "webusb.h"

static tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x057E,
    .idProduct = 0x2009,
    .bcdDevice = 0x0200,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    const usb_identity_t *id = usb_mode_identity();
    s_device.idVendor = id->vid;
    s_device.idProduct = id->pid;
    // Xbox 360 mode is vendor class at device level, like a real one. Linux
    // then doesn't match class-only interface drivers (usbhid) to any of its
    // interfaces, so with the Joy-Con 2 mouse the class is per interface
    // (xpad still matches: it names the vendor too).
    uint8_t cls = usb_mode_has_mouse() ? 0x00 : id->device_class;
    s_device.bDeviceClass = s_device.bDeviceSubClass = s_device.bDeviceProtocol = cls;
    // A distinct bcdDevice keeps Windows from reusing a cached "no MS OS
    // descriptor" answer from a genuine controller.
    s_device.bcdUSB = g_settings.webusb_enabled ? 0x0210 : 0x0200;
    s_device.bcdDevice = (uint16_t)(id->bcd_device | (g_settings.webusb_enabled ? 1 : 0));
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
#define EP_MOUSE_IN   0x83
#define CONFIG_MAX (TUD_CONFIG_DESC_LEN + 64 + TUD_VENDOR_DESC_LEN + USB_MOUSE_ITF_DESC_LEN)

static uint8_t s_config[CONFIG_MAX];

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    bool webusb = g_settings.webusb_enabled;
    // Interface 0: the controller (HID, or XInput in Xbox 360 mode).
    uint16_t n = TUD_CONFIG_DESC_LEN;
    n += usb_mode_interface_desc(s_config + n, (uint16_t)(sizeof s_config - n), 0, EP_IN, EP_OUT);
    uint8_t itfs = 1;
    if (webusb) {
        // Interface 1: WebUSB configuration (vendor class, bulk)
        const uint8_t vendor[] = {TUD_VENDOR_DESCRIPTOR(1, 4, EP_VENDOR_OUT, EP_VENDOR_IN, 64)};
        memcpy(s_config + n, vendor, sizeof vendor);
        n += sizeof vendor;
        itfs++;
    }
    if (usb_mode_has_mouse()) {
        // Last: the Joy-Con 2 mouse (the WebUSB interface keeps its number).
        n += usb_mouse_interface_desc(s_config + n, (uint16_t)(sizeof s_config - n), itfs, EP_MOUSE_IN);
        itfs++;
    }
    // Configuration: bus powered, remote wakeup, 500 mA
    const uint8_t head[] = {9, TUSB_DESC_CONFIGURATION, U16_TO_U8S_LE(n), itfs, 1, 0, 0xA0, 0xFA};
    static_assert(sizeof head == TUD_CONFIG_DESC_LEN, "config header length");
    memcpy(s_config, head, sizeof head);
    return s_config;
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------
static const char *string_for(uint8_t index) {
    switch (index) {
    case 1: return usb_mode_identity()->manufacturer;
    case 2: return usb_mode_identity()->product;
    case 3: return "000000000001";
    case 4: return "Switch2-Pico Config";
    default: return NULL;
    }
}

static uint16_t s_str[48];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;
    if (index == 0) {
        s_str[1] = 0x0409;
        chr_count = 1;
    } else {
        const char *str = string_for(index);
        if (!str) return NULL;
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 47) chr_count = 47;
        for (uint8_t i = 0; i < chr_count; i++) s_str[1 + i] = str[i];
    }
    s_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return s_str;
}
