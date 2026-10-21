// Pro Controller 2 headphone audio on the Pico 2 W: the USB speaker
// (usb_audio.c) fills a PCM ring on core 0, an Opus encoder on core 1 turns
// it into 20 ms frames, and s2_link.c takes those from core 0 and sends them
// (see core/src/headset_audio.h). Stereo CELT at 96 kbit/s CBR, complexity
// 0: about 8.3 ms per frame at 150 MHz with the encoder in RAM.
//
// The host sends 48 kHz by its clock and the frames leave by ours. Core 1
// encodes each frame just in time for s2_link.c to take it (ENCODE_LEAD_US
// before it's due), so it reads the ring at our pace and from the freshest
// audio; the ring's level then tells the drift, and core 1 stretches or
// squeezes a frame by one sample (resampling it) to keep the level near
// TARGET.

#include "headset_audio_pico.h"

#include <math.h>
#include <string.h>

#include "headset_audio.h"
#include "log.h"
#include "opus.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "s2_link.h"
#include "settings.h"
#include "usb_audio.h"
#if S2_HEADSET_MIC
#include "headset_mic_pico.h"
// Core 1 decodes mic frames while it waits, if one fits before the next
// encode (decode about 3 ms).
#define MIC_POLL(room_us) ((room_us) > 4000 && headset_mic_decode_one())
#else
#define MIC_POLL(room_us) false
#endif

#define FRAME S2_AUDIO_FRAME_SAMPLES   // 960 samples per channel
// PCM ring, in stereo samples (L | R << 16): 85 ms.
#define RING_LEN  4096u
#define RING_MASK (RING_LEN - 1)
// Ring level wanted when core 1 reads a frame: the frame plus 5 ms. The
// packets arrive every 1 ms (taken at each start of frame) and core 1 reads
// at a fixed time, so only the host's jitter needs room. Outside +-2.5 ms of
// it, a frame is made from one sample more or less (up to 1000 ppm, far
// more than clocks drift).
#define TARGET    (FRAME + 240)
#define BAND      120
// Core 1 starts a frame this long before it's due: the encode (8.5 ms at
// most) plus 2.5 ms. If it's still being encoded when due, s2_link.c waits
// for it (HEADSET_AUDIO_NOT_YET).
#define ENCODE_LEAD_US 11000
// Keep the controller's stream going this long after the host closes its
// stream: it takes about 0.5 s to come back.
#define LINGER_MS 30000
// A frame taken longer ago than this is stale: s2_link.c wasn't streaming.
#define STALE_MS  100

// Core 1's stack. The SDK's default is 2 KB, and the encoder needs about
// 4 KB (quant_all_bands alone takes 1.8 KB): it overflowed into the heap on
// every frame, corrupting whatever lay there (a hang minutes into a game, or
// at boot, depending on the build). Painted, so the log can show the most
// it has used.
#define CORE1_STACK_BYTES 16384
#define STACK_PAINT       0x5354434bu   // "STCK"
static uint32_t s_core1_stack[CORE1_STACK_BYTES / 4];

typedef struct {
    uint8_t b[S2_AUDIO_FRAME_BYTES];
} frame_t;

static queue_t s_frames;   // one frame, encoded as soon as the last was taken
static volatile bool s_encoding;   // core 1 is making the next one
static volatile uint32_t s_encode_at;   // time_us_32() to start the next one
static volatile uint32_t s_core1_alive;   // counts up while core 1 runs its loop
static volatile int s_encoder_error;   // set by core 1, read (and logged) by core 0

// PCM ring: written by core 0 (s_w), read by core 1 (s_r).
static uint32_t s_ring[RING_LEN];
static volatile uint32_t s_w, s_r;
static volatile bool s_flush;   // core 0 asks core 1 to drop what's in the ring

// Stats, written by one core and logged (then reset) by core 0 every 5 s.
static volatile uint32_t s_enc_frames, s_enc_us_sum, s_enc_us_max;
static volatile uint32_t s_squeezed, s_stretched, s_underruns, s_skipped, s_overflow, s_usb_in;
static uint32_t s_taken, s_missed, s_report_at, s_last_take;
static bool s_usb_stream;
static uint32_t s_usb_stream_end;   // when the host last closed its stream (0: never opened)

#if S2_AUDIO_TEST_TONE
static void fill_pcm(int16_t *pcm) {
    // 440 Hz at -20 dBFS in both channels.
    static float phase;
    const float step = 2.0f * (float)M_PI * 440.0f / 48000.0f;
    for (int i = 0; i < FRAME; i++) {
        int16_t v = (int16_t)(sinf(phase) * 3277.0f);
        pcm[2 * i] = pcm[2 * i + 1] = v;
        phase += step;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    }
}
#else
static void fill_pcm(int16_t *pcm) {
    static bool playing;   // false: waiting for the ring to reach TARGET
    uint32_t r = s_r;
    if (s_flush) {
        s_flush = false;
        r = s_w;
        playing = false;
    }
    uint32_t level = s_w - r;
    __dmb();   // read the samples after the level
    if (!playing && level >= TARGET) {   // start at TARGET exactly
        playing = true;
        r += level - TARGET;
        level = TARGET;
    }
    if (playing && level < FRAME) {
        playing = false;
        s_underruns++;
    }
    if (!playing) {
        s_r = r;
        memset(pcm, 0, FRAME * 2 * sizeof pcm[0]);
        return;
    }
    if (level > TARGET + 2 * FRAME) {   // far ahead (a stall): catch up at once
        s_skipped += level - TARGET;
        r += level - TARGET;
        level = TARGET;
    }
    int adj = 0;   // +1: one sample more into this frame, -1: one less
    if (level > TARGET + BAND) {
        adj = 1;
        s_squeezed++;
    } else if (level < TARGET - BAND) {
        adj = -1;
        s_stretched++;
    }
    if (adj == 0) {
        for (uint32_t i = 0; i < FRAME; i++) {
            uint32_t v = s_ring[(r + i) & RING_MASK];
            pcm[2 * i] = (int16_t)v;
            pcm[2 * i + 1] = (int16_t)(v >> 16);
        }
    } else {
        // Resample FRAME + adj samples to FRAME (linear interpolation), so the
        // correction has no jump in it. Positions in 16.16 fixed point.
        uint32_t step = ((uint32_t)(FRAME + adj) << 16) / FRAME;
        for (uint32_t i = 0, pos = 0; i < FRAME; i++, pos += step) {
            uint32_t a = s_ring[(r + (pos >> 16)) & RING_MASK];
            uint32_t b = s_ring[(r + (pos >> 16) + 1) & RING_MASK];
            int32_t t = (int32_t)(pos & 0xffff);
            int32_t la = (int16_t)a, lb = (int16_t)b, ra = (int16_t)(a >> 16), rb = (int16_t)(b >> 16);
            pcm[2 * i] = (int16_t)(la + (((lb - la) * t) >> 16));
            pcm[2 * i + 1] = (int16_t)(ra + (((rb - ra) * t) >> 16));
        }
    }
    __dmb();   // finish reading before handing the space back
    s_r = r + FRAME + (uint32_t)adj;
}
#endif

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

    static int16_t pcm[FRAME * 2];
    static frame_t f;
    for (;;) {
        // Paced by s2_link.c taking frames (the next encode is then at least
        // 9 ms away: room to decode a mic frame).
        while (!queue_is_empty(&s_frames)) {
            s_core1_alive++;
            if (!MIC_POLL(9000)) tight_loop_contents();
        }
        // Then just in time (when s2_link.c streams; otherwise this is past).
        for (;;) {
            int32_t room = (int32_t)(s_encode_at - time_us_32());
            if (room <= 0 || room >= 25000) break;
            if (!MIC_POLL(room)) tight_loop_contents();
        }
        s_encoding = true;
        fill_pcm(pcm);
        uint32_t t0 = time_us_32();
        int len = opus_encode(enc, pcm, FRAME, f.b, sizeof f.b);
        uint32_t dt = time_us_32() - t0;
        s_enc_frames++;
        s_enc_us_sum += dt;
        if (dt > s_enc_us_max) s_enc_us_max = dt;
        if (len < 0) {
            s_encoder_error = len;
            continue;
        }
        if (len < S2_AUDIO_FRAME_BYTES) memset(f.b + len, 0, S2_AUDIO_FRAME_BYTES - (size_t)len);
        queue_add_blocking(&s_frames, &f);
        s_encoding = false;
    }
}

void headset_audio_init(void) {
#if S2_HEADSET_MIC
    headset_mic_init();
#endif
    queue_init(&s_frames, sizeof(frame_t), 1);
    for (size_t i = 0; i < CORE1_STACK_BYTES / 4; i++) s_core1_stack[i] = STACK_PAINT;
    multicore_launch_core1_with_stack(core1_main, s_core1_stack, sizeof s_core1_stack);
}

// The most core 1's stack has used (bytes), from the paint left at its bottom.
static uint32_t core1_stack_used(void) {
    size_t i = 0;
    while (i < CORE1_STACK_BYTES / 4 && s_core1_stack[i] == STACK_PAINT) i++;
    return (uint32_t)(CORE1_STACK_BYTES - i * 4);
}

int headset_audio_encoder_error(void) {
    return s_encoder_error;
}

void __not_in_flash_func(headset_audio_pcm)(const volatile uint32_t *lr, uint32_t samples) {
    uint32_t w = s_w, r = s_r;
    s_usb_in += samples;
    for (uint32_t i = 0; i < samples; i++) {
        if (w - r >= RING_LEN) {   // full: core 1 isn't reading (no controller)
            s_overflow += samples - i;
            break;
        }
        s_ring[w & RING_MASK] = lr[i];
        w++;
    }
    __dmb();   // the samples before the level
    s_w = w;
}

static void report(uint32_t now);

void headset_audio_usb_stream(bool on) {
    if (on == s_usb_stream) return;
    s_usb_stream = on;
    if (!on) s_usb_stream_end = to_ms_since_boot(get_absolute_time()) | 1;
    s_flush = true;   // a new stream starts from an empty ring
    LOG("audio: USB stream %s", on ? "opened" : "closed");
}

void headset_audio_task(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    // The mic needs the stream to the controller on too.
    bool on = S2_AUDIO_TEST_TONE || usb_audio_streaming() ||
              (s_usb_stream_end && now - s_usb_stream_end < LINGER_MS);
#if S2_HEADSET_MIC
    on = on || headset_mic_open();
#endif
    if (!on) s_usb_stream_end = 0;
    if ((int32_t)(now - s_report_at) >= 30000) report(now);
    s2_link_set_headset_audio(on, g_settings.headset_low_latency ? S2_AUDIO_MODE_LOW_LATENCY : S2_AUDIO_MODE_STABLE);
}

static void report(uint32_t now) {
    // One line every 30 s: the log holds the main loop up (UART).
    static bool stack_logged;
    static uint32_t alive_seen;
    uint32_t alive = s_core1_alive;
    if (alive == alive_seen) LOG("audio: core 1 has stopped (no progress in 30 s)");
    alive_seen = alive;
    uint32_t used = core1_stack_used();
    if (used > CORE1_STACK_BYTES - 1024 && !stack_logged) {
        stack_logged = true;
        LOG("audio: core 1 stack nearly full (%lu of %u bytes used)", (unsigned long)used, CORE1_STACK_BYTES);
    }
    uint32_t n = s_enc_frames;
    if (s_taken || n || s_usb_in) {
        LOG("audio 30 s: %lu sent, %lu missing; enc avg %lu max %lu us, stack %lu; USB in %lu, ring %lu, "
            "+%lu -%lu, %lu under, %lu skip, %lu over",
            (unsigned long)s_taken, (unsigned long)s_missed, (unsigned long)(n ? s_enc_us_sum / n : 0),
            (unsigned long)s_enc_us_max, (unsigned long)used, (unsigned long)s_usb_in, (unsigned long)(s_w - s_r),
            (unsigned long)s_squeezed, (unsigned long)s_stretched, (unsigned long)s_underruns,
            (unsigned long)s_skipped, (unsigned long)s_overflow);
        uint32_t both, step, gap;
        usb_audio_stats(&both, &step, &gap);
        if (both || step || gap) {
            LOG("audio 30 s: USB late %lu, out of step %lu, frames without SOF %lu", (unsigned long)both,
                (unsigned long)step, (unsigned long)gap);
        }
    }
    s_taken = s_missed = 0;
    s_enc_frames = s_enc_us_sum = s_enc_us_max = 0;
    s_squeezed = s_stretched = s_underruns = s_skipped = s_overflow = s_usb_in = 0;
    s_report_at = now;
#if S2_HEADSET_MIC
    headset_mic_report();
#endif
}

headset_audio_take_t headset_audio_take_frame(uint8_t frame[S2_AUDIO_FRAME_BYTES], uint32_t next_due_ms) {
    s_encode_at = (uint32_t)((uint64_t)next_due_ms * 1000u) - ENCODE_LEAD_US;
    static bool logged;
    if (s_encoder_error && !logged) {
        logged = true;
        LOG("audio: Opus encoder error %d", s_encoder_error);
    }
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool stale = now - s_last_take > STALE_MS;
    frame_t f;
    if (!queue_try_remove(&s_frames, &f)) {
        if (s_encoding && !stale) return HEADSET_AUDIO_NOT_YET;
        s_last_take = now;
        s_missed++;
        return HEADSET_AUDIO_NONE;
    }
    s_last_take = now;
    if (stale) return HEADSET_AUDIO_NONE;
    s_taken++;
    memcpy(frame, f.b, S2_AUDIO_FRAME_BYTES);
    return HEADSET_AUDIO_FRAME;
}
