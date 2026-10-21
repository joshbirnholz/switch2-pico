#ifndef S2P_HEADSET_AUDIO_PICO_H
#define S2P_HEADSET_AUDIO_PICO_H

// Pico 2 W side of the headphone audio: the encoder on core 1 and the PCM
// from the USB speaker (usb_audio.c).

#include <stdbool.h>
#include <stdint.h>

void headset_audio_init(void);
// Main loop: turns the controller's stream on while the host streams (and
// for a while after), through s2_link_set_headset_audio().
void headset_audio_task(void);
// Last libopus error on core 1 (0: none), for the log.
int headset_audio_encoder_error(void);

// From the USB stack (core 0): the host opened or closed its stream, and
// (in its interrupt) `samples` 48 kHz stereo samples it sent, one word each
// (L in the low half, R in the high half).
void headset_audio_usb_stream(bool on);
void headset_audio_pcm(const volatile uint32_t *lr, uint32_t samples);

#endif
