#ifndef S2P_HEADSET_AUDIO_H
#define S2P_HEADSET_AUDIO_H

// Pro Controller 2 headphone jack audio (see research/headset-audio.md).
// s2_link.c sets the stream up and writes the frames; the board encodes
// them (Opus CELT, 48 kHz stereo, 20 ms, CBR S2_AUDIO_FRAME_BYTES).

#include <stdbool.h>
#include <stdint.h>

#include "s2_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- implemented by the board (weak default in s2_link.c: none) ----
typedef enum {
    HEADSET_AUDIO_NONE,      // no frame: s2_link.c sends silence
    HEADSET_AUDIO_FRAME,     // `frame` filled
    HEADSET_AUDIO_NOT_YET,   // being encoded: ask again shortly
} headset_audio_take_t;
// The next encoded 20 ms frame.
headset_audio_take_t headset_audio_take_frame(uint8_t frame[S2_AUDIO_FRAME_BYTES]);

#ifdef __cplusplus
}
#endif

#endif
