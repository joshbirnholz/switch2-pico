#ifndef S2P_USB_AUDIO_H
#define S2P_USB_AUDIO_H

// USB Audio Class 2.0 headphone playback and optional headset capture.

#include <stdbool.h>
#include <stdint.h>

#include "device/usbd_pvt.h"

#include "usb_audio2.h"
#define USB_AUDIO_ITF_COUNT AUDIO2_INTERFACE_COUNT
#define USB_AUDIO_DESC_LEN AUDIO2_DESCRIPTOR_LENGTH

// Whether the configuration has the speaker (latched at boot like the USB mode).
bool usb_audio_present(void);
// Interface association, AudioControl `itf` and AudioStreaming `itf` + 1
// (and with S2_HEADSET_MIC, the microphone's at `itf` + 2, endpoint `ep_in`).
uint16_t usb_audio_desc(uint8_t *buf, uint16_t cap, uint8_t itf, uint8_t ep_out, uint8_t ep_in, uint8_t ep_notify, uint8_t str_idx);
// Main loop: publish connector-state changes without USB re-enumeration.
void usb_audio_task(void);
// The host has the stream open (alternate setting 1).
bool usb_audio_streaming(void);
// Either streaming interface has alternate setting 1 on an active host.
bool usb_audio_active(void);

// Since the last call: start-of-frame callbacks that found both buffers
// full, that found them out of step, and frames with no callback.
void usb_audio_stats(uint32_t *both_full, uint32_t *out_of_step, uint32_t *frames_skipped);

extern const usbd_class_driver_t usb_audio_driver;

#endif
