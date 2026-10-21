// Opus benchmark for the RP2350: how long does it take to encode the
// controller's headphone frames (CELT, 48 kHz stereo, 20 ms, CBR 240 B =
// 96 kbit/s) and to decode its mic frames (CELT, mono, 20 ms, 100 B)?
// Prints the results over USB serial, then repeats them every 10 s.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "opus.h"
#include "pico/stdlib.h"

#define RATE 48000
#define FRAME 960
#define FRAMES 100

static int16_t pcm[FRAME * 2];
static uint8_t pkt[1500];
static uint8_t mic_pkts[FRAMES][100];
static char report[4096];
static size_t report_len;

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report_len += vsnprintf(report + report_len, sizeof report - report_len, fmt, ap);
    va_end(ap);
}

// Music-like test signal: a few tones plus noise, different in each channel.
static void fill(int16_t *out, int channels, int frame_no) {
    static uint32_t lcg = 12345;
    for (int i = 0; i < FRAME; i++) {
        int n = frame_no * FRAME + i;
        float t = (float)n / RATE;
        for (int c = 0; c < channels; c++) {
            lcg = lcg * 1664525u + 1013904223u;
            float noise = (float)(int32_t)lcg / 2147483648.0f;
            float v = 0.2f * __builtin_sinf(6.2831853f * (220.0f + 110.0f * c) * t) +
                      0.1f * __builtin_sinf(6.2831853f * 1330.0f * t) + 0.05f * noise;
            out[i * channels + c] = (int16_t)(v * 32767.0f);
        }
    }
}

static void bench_encode(int complexity) {
    int err;
    OpusEncoder *enc = opus_encoder_create(RATE, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (err != OPUS_OK) {
        say("  encoder create failed: %d\n", err);
        return;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(96000));
    opus_encoder_ctl(enc, OPUS_SET_VBR(0));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
    uint64_t total = 0, worst = 0;
    int len = 0;
    for (int f = 0; f < FRAMES; f++) {
        fill(pcm, 2, f);
        uint64_t t0 = time_us_64();
        len = opus_encode(enc, pcm, FRAME, pkt, sizeof pkt);
        uint64_t dt = time_us_64() - t0;
        total += dt;
        if (dt > worst) worst = dt;
    }
    say("  encode stereo, complexity %2d: avg %5.2f ms, worst %5.2f ms per 20 ms frame (%d B, TOC %02x)\n",
        complexity, total / 1000.0 / FRAMES, worst / 1000.0, len, pkt[0]);
    opus_encoder_destroy(enc);
}

static void bench_decode(void) {
    int err;
    // Make mic-like frames first: mono, CBR 100 B (40 kbit/s).
    OpusEncoder *enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(40000));
    opus_encoder_ctl(enc, OPUS_SET_VBR(0));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));
    for (int f = 0; f < FRAMES; f++) {
        fill(pcm, 1, f);
        opus_encode(enc, pcm, FRAME, mic_pkts[f], 100);
    }
    opus_encoder_destroy(enc);
    OpusDecoder *dec = opus_decoder_create(RATE, 1, &err);
    uint64_t total = 0, worst = 0;
    for (int f = 0; f < FRAMES; f++) {
        uint64_t t0 = time_us_64();
        opus_decode(dec, mic_pkts[f], 100, pcm, FRAME, 0);
        uint64_t dt = time_us_64() - t0;
        total += dt;
        if (dt > worst) worst = dt;
    }
    say("  decode mono 100 B (mic):       avg %5.2f ms, worst %5.2f ms per 20 ms frame\n",
        total / 1000.0 / FRAMES, worst / 1000.0);
    opus_decoder_destroy(dec);
}

int main(void) {
    stdio_init_all();
    say("opus_bench: libopus %s, fixed point, encoder state %d B (stereo), decoder %d B (mono)\n",
        opus_get_version_string(), opus_encoder_get_size(2), opus_decoder_get_size(1));
    static const uint32_t khz[] = {150000, 200000};
    for (unsigned k = 0; k < sizeof khz / sizeof khz[0]; k++) {
        if (!set_sys_clock_khz(khz[k], false)) {
            say("\n%lu MHz: not available\n", (unsigned long)(khz[k] / 1000));
            continue;
        }
        say("\n%lu MHz:\n", (unsigned long)(khz[k] / 1000));
        static const int cx[] = {0, 2, 5, 10};
        for (unsigned i = 0; i < 4; i++) bench_encode(cx[i]);
        bench_decode();
    }
    set_sys_clock_khz(150000, false);
    say("\nbudget: 20 ms per frame in real time; the encoder will run on the second core.\n");
    for (;;) {
        printf("\n----\n%s", report);
        sleep_ms(10000);
    }
}
