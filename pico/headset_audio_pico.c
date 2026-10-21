// Pro Controller 2 headphone audio on the Pico 2 W: an Opus encoder on core 1
// fills a small queue of 20 ms frames, which s2_link.c takes from core 0 (see
// core/src/headset_audio.h). Stereo CELT at 96 kbit/s CBR, complexity 0:
// about 9.4 ms per frame at 150 MHz (research/audio/opus_bench).
//
// For now the audio is a test tone (S2_AUDIO_TEST_TONE) or silence; the USB
// audio interface will feed it later.

#include "headset_audio_pico.h"

#include <math.h>
#include <string.h>

#include "headset_audio.h"
#include "log.h"
#include "opus.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"

#define QUEUE_FRAMES 3   // 60 ms of encoded audio ahead

typedef struct {
    uint8_t b[S2_AUDIO_FRAME_BYTES];
} frame_t;

static queue_t s_frames;
static volatile int s_encoder_error;   // set by core 1, read (and logged) by core 0
// Encoder timing, written by core 1 and logged (then reset) by core 0.
static volatile uint32_t s_enc_frames, s_enc_us_sum, s_enc_us_max;
static uint32_t s_taken, s_missed, s_report_at;

static void core1_main(void) {
    // Flash writes (settings, firmware staging) park this core in RAM.
    multicore_lockout_victim_init();
    int err;
    OpusEncoder *enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (err != OPUS_OK) {
        s_encoder_error = err;
        for (;;) tight_loop_contents();
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(S2_AUDIO_FRAME_BYTES * 8 * 50));
    opus_encoder_ctl(enc, OPUS_SET_VBR(0));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));

    static int16_t pcm[S2_AUDIO_FRAME_SAMPLES * 2];
    static frame_t f;
    float phase = 0.0f;
    for (;;) {
#if S2_AUDIO_TEST_TONE
        // 440 Hz at -20 dBFS in both channels.
        const float step = 2.0f * (float)M_PI * 440.0f / 48000.0f;
        for (int i = 0; i < S2_AUDIO_FRAME_SAMPLES; i++) {
            int16_t v = (int16_t)(sinf(phase) * 3277.0f);
            pcm[2 * i] = pcm[2 * i + 1] = v;
            phase += step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
#else
        memset(pcm, 0, sizeof pcm);
#endif
        uint32_t t0 = time_us_32();
        int len = opus_encode(enc, pcm, S2_AUDIO_FRAME_SAMPLES, f.b, sizeof f.b);
        uint32_t dt = time_us_32() - t0;
        s_enc_frames++;
        s_enc_us_sum += dt;
        if (dt > s_enc_us_max) s_enc_us_max = dt;
        if (len < 0) {
            s_encoder_error = len;
            continue;
        }
        if (len < S2_AUDIO_FRAME_BYTES) memset(f.b + len, 0, S2_AUDIO_FRAME_BYTES - (size_t)len);
        queue_add_blocking(&s_frames, &f);   // paced by s2_link.c taking frames
    }
}

void headset_audio_init(void) {
    queue_init(&s_frames, sizeof(frame_t), QUEUE_FRAMES);
    multicore_launch_core1(core1_main);
}

int headset_audio_encoder_error(void) {
    return s_encoder_error;
}

bool headset_audio_take_frame(uint8_t frame[S2_AUDIO_FRAME_BYTES]) {
    static bool logged;
    if (s_encoder_error && !logged) {
        logged = true;
        LOG("audio: Opus encoder error %d", s_encoder_error);
    }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((int32_t)(now - s_report_at) >= 5000) {
        uint32_t n = s_enc_frames;
        if (s_taken || n) {
            LOG("audio: 5 s: %lu frames sent, %lu missing (silence sent); encoder %lu frames, avg %lu us, max %lu us",
                (unsigned long)s_taken, (unsigned long)s_missed, (unsigned long)n,
                (unsigned long)(n ? s_enc_us_sum / n : 0), (unsigned long)s_enc_us_max);
        }
        s_taken = s_missed = 0;
        s_enc_frames = s_enc_us_sum = s_enc_us_max = 0;
        s_report_at = now;
    }
    frame_t f;
    if (!queue_try_remove(&s_frames, &f)) {
        s_missed++;
        return false;
    }
    s_taken++;
    memcpy(frame, f.b, S2_AUDIO_FRAME_BYTES);
    return true;
}
