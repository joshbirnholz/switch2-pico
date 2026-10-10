// The BOS descriptor (WebUSB landing page, Microsoft OS 2.0 descriptors) and
// the vendor requests that go with it, for both boards.
//
// Microsoft OS 2.0 descriptors tell Windows which driver each interface
// gets: WinUSB for the configuration interface (so Chrome can open it), and,
// in Xbox 360 mode, Windows' own Xbox 360 controller driver (compatible ID
// XUSB10) for the controller interface. Xbox 360 mode has its own USB ID
// (see usb_mode.c) for that: with Microsoft's ID, that driver takes the
// whole device, the configuration interface and the mice included.

#include <string.h>

#include "tusb.h"

#include "usb_mode.h"
#include "webusb.h"

#define LANDING_URL "joshbirnholz.github.io/switch2-pico/"

static const struct {
    uint8_t bLength, bDescriptorType, bScheme;
    char url[sizeof LANDING_URL - 1];
} __attribute__((packed)) s_url = {3 + sizeof LANDING_URL - 1, 3, 1, LANDING_URL};

// Function subset (8) + compatible ID (20) + DeviceInterfaceGUIDs (132).
static const uint8_t WINUSB_FUNCTION[] = {
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), 0 /* interface */, 0,
    U16_TO_U8S_LE(0x00A0),
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    // Registry property: DeviceInterfaceGUIDs = {8f3b2c66-5c1a-4b5e-9a3e-2b6f53c0d7a1}
    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY), U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0,
    'e', 0, 'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    U16_TO_U8S_LE(0x0050),
    '{', 0, '8', 0, 'f', 0, '3', 0, 'b', 0, '2', 0, 'c', 0, '6', 0, '6', 0, '-', 0, '5', 0, 'c', 0, '1', 0, 'a', 0,
    '-', 0, '4', 0, 'b', 0, '5', 0, 'e', 0, '-', 0, '9', 0, 'a', 0, '3', 0, 'e', 0, '-', 0, '2', 0, 'b', 0, '6', 0,
    'f', 0, '5', 0, '3', 0, 'c', 0, '0', 0, 'd', 0, '7', 0, 'a', 0, '1', 0, '}', 0, 0, 0, 0, 0,
};
_Static_assert(sizeof WINUSB_FUNCTION == 0xA0, "WinUSB function subset length");

// Function subset (8) + compatible ID XUSB10 (20): Windows' Xbox 360 driver.
static const uint8_t XUSB_FUNCTION[] = {
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), 0 /* interface */, 0,
    U16_TO_U8S_LE(0x001C),
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'X', 'U', 'S', 'B', '1', '0', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};
_Static_assert(sizeof XUSB_FUNCTION == 0x1C, "XUSB function subset length");

#define SET_HEAD_LEN 0x0A
#define CONFIG_HEAD_LEN 0x08
#define MS_OS_MAX (SET_HEAD_LEN + CONFIG_HEAD_LEN + sizeof XUSB_FUNCTION + sizeof WINUSB_FUNCTION)

// The controller interface (0) goes to the Xbox 360 driver in Xbox 360 mode.
static bool xinput(void) {
    return usb_mode_active() == USB_MODE_XBOX360 && !usb_mode_config_only();
}

static uint16_t ms_os_len(void) {
    return (uint16_t)(SET_HEAD_LEN + CONFIG_HEAD_LEN + (xinput() ? sizeof XUSB_FUNCTION : 0) + sizeof WINUSB_FUNCTION);
}

static const uint8_t *ms_os_desc(void) {
    static uint8_t d[MS_OS_MAX];
    uint16_t total = ms_os_len(), n = 0;
    const uint8_t head[] = {
        // Set header: length, type, Windows version (8.1+), total length
        U16_TO_U8S_LE(SET_HEAD_LEN), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000),
        U16_TO_U8S_LE(total),
        // Configuration subset header
        U16_TO_U8S_LE(CONFIG_HEAD_LEN), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0,
        U16_TO_U8S_LE(total - SET_HEAD_LEN),
    };
    memcpy(d, head, sizeof head);
    n = sizeof head;
    if (xinput()) {
        memcpy(d + n, XUSB_FUNCTION, sizeof XUSB_FUNCTION);
        d[n + 4] = 0;
        n += sizeof XUSB_FUNCTION;
    }
    memcpy(d + n, WINUSB_FUNCTION, sizeof WINUSB_FUNCTION);
    // The configuration interface: 1 after the controller, 0 when it's the
    // only one (configuration-only device).
    d[n + 4] = usb_mode_config_only() ? 0 : 1;
    return d;
}

const uint8_t *usb_bos_descriptor(void) {
    static uint8_t bos[TUD_BOS_DESC_LEN + TUD_BOS_WEBUSB_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN];
    const uint8_t d[] = {
        TUD_BOS_DESCRIPTOR(sizeof bos, 2),
        TUD_BOS_WEBUSB_DESCRIPTOR(WEBUSB_VENDOR_REQUEST_URL, 1),
        TUD_BOS_MS_OS_20_DESCRIPTOR(ms_os_len(), WEBUSB_VENDOR_REQUEST_MS),
    };
    memcpy(bos, d, sizeof bos);
    return bos;
}

// Vendor control requests: the WebUSB landing page and the Microsoft OS 2.0
// descriptors. (On the nRF52 this replaces Adafruit_USBD_WebUSB's own.)
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) return false;
    switch (request->bRequest) {
    case WEBUSB_VENDOR_REQUEST_URL:
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)&s_url, s_url.bLength);
    case WEBUSB_VENDOR_REQUEST_MS:
        if (request->wIndex == 7) return tud_control_xfer(rhport, request, (void *)(uintptr_t)ms_os_desc(), ms_os_len());
        return false;
    default:
        return false;
    }
}
