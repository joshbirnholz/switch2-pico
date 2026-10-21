#ifndef S2P_HEADSET_AUDIO_PICO_H
#define S2P_HEADSET_AUDIO_PICO_H

// Pico 2 W side of the headphone audio: starts the encoder on core 1.

void headset_audio_init(void);
// Last libopus error on core 1 (0: none), for the log.
int headset_audio_encoder_error(void);

#endif
