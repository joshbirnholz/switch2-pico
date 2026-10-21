#ifndef S2P_USB_AUDIO_H
#define S2P_USB_AUDIO_H

// USB speaker for the Pro Controller 2's headphone jack: a USB Audio Class
// 1.0 function (48 kHz, 16-bit stereo, adaptive isochronous OUT), two
// interfaces after the others. The PCM goes to headset_audio_pico.c.

#include <stdbool.h>
#include <stdint.h>

#include "device/usbd_pvt.h"

#define USB_AUDIO_ITF_COUNT 2
#define USB_AUDIO_DESC_LEN  99

// After tud_init(): installs the endpoint's interrupt handler.
void usb_audio_init(void);
// Whether the configuration has the speaker (latched at boot like the USB mode).
bool usb_audio_present(void);
// Interface association, AudioControl `itf` and AudioStreaming `itf` + 1.
uint16_t usb_audio_desc(uint8_t *buf, uint16_t cap, uint8_t itf, uint8_t ep_out, uint8_t str_idx);
// The host has the stream open (alternate setting 1).
bool usb_audio_streaming(void);

extern const usbd_class_driver_t usb_audio_driver;

#endif
