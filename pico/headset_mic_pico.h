#ifndef S2P_HEADSET_MIC_PICO_H
#define S2P_HEADSET_MIC_PICO_H

// Pico 2 W side of the headset mic (S2_HEADSET_MIC): the decoder on core 1
// and the PCM for the USB microphone (usb_audio.c).

#include <stdbool.h>
#include <stdint.h>

void headset_mic_init(void);
// Core 1: decode one queued mic frame (about 3 ms); false when none.
bool headset_mic_decode_one(void);
// USB interrupt: the next packet's samples (47-49; zeros until there's
// audio). Returns how many.
uint32_t headset_mic_read(int16_t *out, uint32_t max);
// USB stack: the host opened or closed the microphone.
void headset_audio_usb_mic(bool on);
bool headset_mic_open(void);
// Main loop, every 30 s: one log line.
void headset_mic_report(void);

#endif
