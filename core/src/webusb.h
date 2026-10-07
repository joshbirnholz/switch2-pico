#ifndef S2P_WEBUSB_H
#define S2P_WEBUSB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// WebUSB configuration channel. The configuration page (opened from the
// landing page URL Chrome shows on plug-in, or from a local copy of
// web/index.html) sends the same requests it would send over HTTP:
//
//   host -> device: "S2PQ" u32le(len) "<METHOD> <path>[?query]\n<body>"
//   device -> host: "S2PR" u16le(status) u16le(0) u32le(len) <body>
//
// so both transports share web_api.c.

#define WEBUSB_VENDOR_REQUEST_URL 1
#define WEBUSB_VENDOR_REQUEST_MS  2

void webusb_task(void);

#ifdef __cplusplus
}
#endif

#endif
