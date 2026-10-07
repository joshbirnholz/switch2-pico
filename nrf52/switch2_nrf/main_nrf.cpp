// Switch2-Pico on nRF52840: Arduino entry points, USB identity and status LED.

#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <InternalFileSystem.h>
#include <bluefruit.h>

#include "app.h"
#include "app_core.h"
#include "log.h"
#include "platform.h"
#include "s2_link.h"
#include "settings.h"
#include "usb_hid.h"
#include "version.h"

// ---------------------------------------------------------------------------
// Status LED. Pro Micro nRF52840 / nice!nano clones built with the Feather
// board definition have their LED on P0.15 (Arduino pin 24); real Feathers
// use LED_BUILTIN. Driving both covers either board.
// ---------------------------------------------------------------------------
#ifndef S2P_LED_PIN_A
#define S2P_LED_PIN_A LED_BUILTIN
#endif
#ifndef S2P_LED_PIN_B
#define S2P_LED_PIN_B 24
#endif
#ifndef S2P_LED_ON
#define S2P_LED_ON HIGH
#endif

static void led_write(bool on) {
    int level = on ? S2P_LED_ON : !S2P_LED_ON;
    digitalWrite(S2P_LED_PIN_A, level);
    digitalWrite(S2P_LED_PIN_B, level);
}

static void status_led_task(void) {
    static int last = -1;
    uint32_t t = millis();
    bool on;
    switch (s2_link_state()) {
    case S2_LINK_READY:
        on = true;
        break;
    case S2_LINK_CONNECTING:
    case S2_LINK_DISCOVERING:
    case S2_LINK_INITIALISING:
        on = (t / 60) & 1;
        break;
    case S2_LINK_SCANNING: {
        uint8_t stage, reason;
        uint32_t age;
        if (s2_link_last_failure(&stage, &reason, &age) && age < 60000) {
            uint32_t ph = t % 3000;   // N quick blinks = failed stage
            on = ph < stage * 400u && (ph % 400) < 150;
        } else {
            on = g_settings.bonded ? (t % 2000) < 150 : ((t / 250) & 1);
        }
        break;
    }
    default:
        on = false;
        break;
    }
    if ((int)on != last) {
        last = on;
        led_write(on);
    }
}

// ---------------------------------------------------------------------------
// USB: present a wired Switch Pro Controller (057E:2009). Interface 0 is our
// own HID class driver (core/src/usb_hid.c); interface 1 is the optional
// WebUSB configuration channel.
// ---------------------------------------------------------------------------
class ProControllerInterface : public Adafruit_USBD_Interface {
public:
    uint16_t getInterfaceDescriptor(uint8_t itfnum_deprecated, uint8_t *buf, uint16_t bufsize) override {
        (void)itfnum_deprecated;
        const uint16_t len = 9 + 9 + 7 + 7;
        if (!buf) return len;
        if (bufsize < len) return 0;
        uint8_t itf = TinyUSBDevice.allocInterface(1);
        uint8_t ep_in = TinyUSBDevice.allocEndpoint(TUSB_DIR_IN);
        uint8_t ep_out = TinyUSBDevice.allocEndpoint(TUSB_DIR_OUT);
        uint16_t rlen;
        usb_hid_report_descriptor(&rlen);
        uint8_t interval = g_settings.usb_report_interval_ms ? g_settings.usb_report_interval_ms : 8;
        if (interval > 8) interval = 8;
        const uint8_t desc[] = {
            9, TUSB_DESC_INTERFACE, itf, 0, 2, TUSB_CLASS_HID, 0, 0, 0,
            9, HID_DESC_TYPE_HID, U16_TO_U8S_LE(0x0111), 0, 1, HID_DESC_TYPE_REPORT, U16_TO_U8S_LE(rlen),
            7, TUSB_DESC_ENDPOINT, ep_in, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(USB_HID_EP_SIZE), interval,
            7, TUSB_DESC_ENDPOINT, ep_out, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(USB_HID_EP_SIZE), interval,
        };
        memcpy(buf, desc, len);
        return len;
    }
};

static ProControllerInterface s_pro_itf;
static Adafruit_USBD_WebUSB s_webusb;
WEBUSB_URL_DEF(s_landing_page, 1 /* https */, "joshbirnholz.github.io/switch2-pico/");

static void usb_setup(void) {
    // The core has already enumerated a CDC serial port; replace the whole
    // configuration with ours. Only touch the pull-up once the stack is
    // mounted: toggling it before the USBD peripheral's READY event makes
    // the nRF driver skip its initialisation and USB never comes up.
    if (TinyUSBDevice.mounted()) {
        TinyUSBDevice.detach();
        delay(10);
    }
    TinyUSBDevice.clearConfiguration();
    TinyUSBDevice.setID(0x057E, 0x2009);
    TinyUSBDevice.setManufacturerDescriptor("Nintendo Co., Ltd.");
    TinyUSBDevice.setProductDescriptor("Pro Controller");
    TinyUSBDevice.setSerialDescriptor("000000000001");
    TinyUSBDevice.addInterface(s_pro_itf);
    if (g_settings.webusb_enabled) {
        s_webusb.setLandingPage(&s_landing_page);
        s_webusb.setStringDescriptor("Switch2-Pico Config");
        s_webusb.begin();                     // also switches to USB 2.1 for the BOS descriptor
        TinyUSBDevice.setDeviceVersion(0x0201);
    } else {
        TinyUSBDevice.setVersion(0x0200);
        TinyUSBDevice.setDeviceVersion(0x0200);
    }
    if (!g_settings.usb_detach_when_idle) TinyUSBDevice.attach();
}

extern "C" void app_wifi_stop(void) {
    // No Wi-Fi on this board.
}

void setup() {
    pinMode(S2P_LED_PIN_A, OUTPUT);
    pinMode(S2P_LED_PIN_B, OUTPUT);
    led_write(false);
#ifdef S2P_UART_LOG
    Serial1.begin(115200);
#endif
    log_init();
    LOG("switch2-pico %s starting on nRF52840", S2P_VERSION);
    InternalFS.begin();
    settings_init();
    usb_setup();
    app_core_init();
}

void loop() {
    app_core_task();
    status_led_task();
    // Let the USB and Bluetooth tasks run.
    yield();
}
