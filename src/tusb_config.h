#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED         1
#define CFG_TUD_ENDPOINT0_SIZE  64

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

// The Pro Controller HID interface is implemented by our own class driver
// (src/usb_hid.c), so none of the stock classes are enabled.
#define CFG_TUD_HID 0
#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 0
#define CFG_TUD_MIDI 0
// WebUSB configuration interface (second interface, optional at runtime).
#define CFG_TUD_VENDOR 1
#define CFG_TUD_VENDOR_EPSIZE 64
#define CFG_TUD_VENDOR_RX_BUFSIZE 512
#define CFG_TUD_VENDOR_TX_BUFSIZE 2048

#endif
