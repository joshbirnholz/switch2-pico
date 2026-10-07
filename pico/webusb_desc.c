// WebUSB landing page and Microsoft OS 2.0 descriptors for the Pico build.
// (The nRF52 build gets these from Adafruit_USBD_WebUSB.)

#include <string.h>

#include "tusb.h"

#include "webusb.h"

// ---------------------------------------------------------------------------
// Vendor control requests: WebUSB landing page and Microsoft OS 2.0 descriptors
// (so Windows binds WinUSB to the configuration interface automatically).
// ---------------------------------------------------------------------------
#define LANDING_URL "joshbirnholz.github.io/switch2-pico/"

static const struct {
    uint8_t bLength, bDescriptorType, bScheme;
    char url[sizeof LANDING_URL - 1];
} __attribute__((packed)) s_url = {3 + sizeof LANDING_URL - 1, 3, 1, LANDING_URL};

#define MS_OS_20_DESC_LEN 0xB2
#define WEBUSB_ITF 1

static const uint8_t s_ms_os_20[] = {
    // Set header: length, type, Windows version (8.1+), total length
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000),
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
    // Configuration subset header
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),
    // Function subset header for the WebUSB interface
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), WEBUSB_ITF, 0,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),
    // Compatible ID: WINUSB
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    // Registry property: DeviceInterfaceGUIDs = {8f3b2c66-5c1a-4b5e-9a3e-2b6f53c0d7a1}
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0, 'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0,
    'e', 0, 'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    U16_TO_U8S_LE(0x0050),
    '{', 0, '8', 0, 'f', 0, '3', 0, 'b', 0, '2', 0, 'c', 0, '6', 0, '6', 0, '-', 0, '5', 0, 'c', 0, '1', 0, 'a', 0,
    '-', 0, '4', 0, 'b', 0, '5', 0, 'e', 0, '-', 0, '9', 0, 'a', 0, '3', 0, 'e', 0, '-', 0, '2', 0, 'b', 0, '6', 0,
    'f', 0, '5', 0, '3', 0, 'c', 0, '0', 0, 'd', 0, '7', 0, 'a', 0, '1', 0, '}', 0, 0, 0, 0, 0,
};
_Static_assert(sizeof s_ms_os_20 == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor length");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) return false;
    switch (request->bRequest) {
    case WEBUSB_VENDOR_REQUEST_URL:
        return tud_control_xfer(rhport, request, (void *)(uintptr_t)&s_url, s_url.bLength);
    case WEBUSB_VENDOR_REQUEST_MS:
        if (request->wIndex == 7) {
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_ms_os_20, sizeof s_ms_os_20);
        }
        return false;
    default:
        return false;
    }
}
