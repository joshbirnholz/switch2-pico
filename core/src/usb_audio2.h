#ifndef S2P_USB_AUDIO2_H
#define S2P_USB_AUDIO2_H
#include <stdbool.h>
#include <stdint.h>

#define AUDIO2_TERMINAL_USB_OUT 1
#define AUDIO2_TERMINAL_HEADPHONES 2
#define AUDIO2_TERMINAL_MIC 3
#define AUDIO2_TERMINAL_USB_IN 4
#define AUDIO2_CLOCK_PLAYBACK 5
#define AUDIO2_CLOCK_CAPTURE 6
#define AUDIO2_CONNECTOR_CONTROL 2
#if S2_HEADSET_MIC
#define AUDIO2_INTERFACE_COUNT 3
#define AUDIO2_DESCRIPTOR_LENGTH 217
#else
#define AUDIO2_INTERFACE_COUNT 2
#define AUDIO2_DESCRIPTOR_LENGTH 125
#endif

uint16_t audio2_descriptor(uint8_t *buf, uint16_t cap, uint8_t itf,
                          uint8_t ep_out, uint8_t ep_in, uint8_t ep_notify, uint8_t str);
// Read-only UAC2 entity controls. Zero means unsupported; caller stalls EP0.
uint16_t audio2_get(uint8_t entity, uint8_t selector, uint8_t channel, uint8_t request,
                    bool headphones, bool microphone, uint8_t *out, uint16_t cap);
void audio2_notification(uint8_t out[6], uint8_t itf, uint8_t terminal);
#endif
