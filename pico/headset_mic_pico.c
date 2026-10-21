// Pro Controller 2 headset mic on the Pico 2 W (S2_HEADSET_MIC): mic frames
// from the controller (Opus CELT FB 20 ms mono, 100 bytes; s2_link.c joins
// their halves) are decoded on core 1, between its encodes, into a PCM ring
// that the USB microphone (usb_audio.c) reads at each start of frame.
//
// The controller sends by its clock and the host takes by its own. The
// microphone's endpoint is asynchronous: each packet carries 47, 48 or 49
// samples, whatever keeps the ring near MIC_TARGET, and the host follows.

#include "headset_mic_pico.h"

#include <string.h>

#include "headset_audio.h"
#include "log.h"
#include "opus.h"
#include "pico/util/queue.h"
#include "s2_link.h"

#define MIC_RING_LEN  4096u   // samples (85 ms)
#define MIC_RING_MASK (MIC_RING_LEN - 1)
// Frames come 960 samples at a time every 20 ms: keep a frame plus 10 ms.
#define MIC_TARGET    (S2_MIC_FRAME_SAMPLES + 480)
#define MIC_BAND      240
// The controller's mic is quiet (speech at about -40 dBFS, peaks about -20,
// measured 2026-10-21); the Switch must add gain too. +12 dB, clipped.
#define MIC_GAIN      4
// The controller's mic starts with a full-scale pop that settles in about
// 350 ms (measured 2026-10-21); drop its first 400 ms.
#define MIC_SKIP_FRAMES 20
// The level jumps by a frame every 20 ms; steer by its average over about
// 128 ms (the filter's shift), in 1/256 samples.
#define MIC_AVG_SHIFT 7

typedef struct {
    uint8_t b[S2_MIC_FRAME_BYTES];
} mic_frame_t;

static queue_t s_mic_in;   // from the main loop to core 1
static volatile bool s_open;   // the host records
// PCM ring: written by core 1 (s_w), read in the USB interrupt (s_r).
static int16_t s_ring[MIC_RING_LEN];
static volatile uint32_t s_w, s_r;
static volatile bool s_flush;   // the USB side asks core 1 to start over
static volatile int s_decoder_error;
// Stats, logged by headset_mic_report().
static volatile uint32_t s_frames, s_dropped, s_bad, s_underruns, s_short, s_long;

void headset_mic_init(void) {
    queue_init(&s_mic_in, sizeof(mic_frame_t), 4);
}

void headset_audio_mic_frame(const uint8_t *opus, size_t len) {
    if (!s_open || len != S2_MIC_FRAME_BYTES) return;
    mic_frame_t f;
    memcpy(f.b, opus, sizeof f.b);
    if (!queue_try_add(&s_mic_in, &f)) s_dropped++;
}

bool headset_mic_decode_one(void) {
    static OpusDecoder *dec;
    static int16_t pcm[S2_MIC_FRAME_SAMPLES];
    mic_frame_t f;
    if (!queue_try_remove(&s_mic_in, &f)) return false;
    if (!dec) {
        int err;
        dec = opus_decoder_create(48000, 1, &err);
        if (err != OPUS_OK) {
            s_decoder_error = err;
            dec = NULL;
            return false;
        }
    }
    int n = opus_decode(dec, f.b, sizeof f.b, pcm, S2_MIC_FRAME_SAMPLES, 0);
    if (n <= 0) {
        s_bad++;
        return true;
    }
    static int skip;
    uint32_t w = s_w;
    if (s_flush) {   // only core 1 moves s_w; the reader waits for this
        s_flush = false;
        w = s_r;
        skip = MIC_SKIP_FRAMES;
    }
    if (skip > 0) {
        skip--;
        s_w = w;
        s_frames++;
        return true;
    }
    for (int i = 0; i < n; i++) {
        if (w - s_r >= MIC_RING_LEN) break;   // full: the host isn't reading
        int32_t v = pcm[i] * MIC_GAIN;
        s_ring[w & MIC_RING_MASK] = (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
        w++;
    }
    __dmb();   // the samples before the level
    s_w = w;
    s_frames++;
    return true;
}

uint32_t __not_in_flash_func(headset_mic_read)(int16_t *out, uint32_t max) {
    static bool playing;   // false: waiting for the ring to reach MIC_TARGET
    static int32_t avg;    // the level's average, in 1/256 samples
    uint32_t n = 48;
    uint32_t r = s_r, level = s_w - r;
    __dmb();
    if (s_flush) level = 0;
    if (!playing && level >= MIC_TARGET) {
        playing = true;
        avg = (int32_t)level << 8;
    }
    if (playing && level < 49) {
        playing = false;
        s_underruns++;
    }
    if (!playing) {
        memset(out, 0, n * sizeof out[0]);
        return n;
    }
    avg += (((int32_t)level << 8) - avg) >> MIC_AVG_SHIFT;
    if (avg > (MIC_TARGET + MIC_BAND) << 8) {
        n = 49;
        s_long++;
    } else if (avg < (MIC_TARGET - MIC_BAND) << 8) {
        n = 47;
        s_short++;
    }
    if (n > max) n = max;
    for (uint32_t i = 0; i < n; i++) out[i] = s_ring[(r + i) & MIC_RING_MASK];
    __dmb();
    s_r = r + n;
    return n;
}

void headset_audio_usb_mic(bool on) {
    if (on == s_open) return;
    s_open = on;
    s_flush = true;
    s2_link_set_headset_mic(on);
    LOG("audio: USB mic %s", on ? "opened" : "closed");
}

bool headset_mic_open(void) {
    return s_open;
}

void headset_mic_report(void) {
    if (s_decoder_error) LOG("audio: Opus decoder error %d", s_decoder_error);
    if (!s_open && !s_frames) return;
    LOG("mic 30 s: %lu frames, %lu dropped, %lu bad; ring %lu, %lu under, %lu short, %lu long",
        (unsigned long)s_frames, (unsigned long)s_dropped, (unsigned long)s_bad, (unsigned long)(s_w - s_r),
        (unsigned long)s_underruns, (unsigned long)s_short, (unsigned long)s_long);
    s_frames = s_dropped = s_bad = s_underruns = s_short = s_long = 0;
}
