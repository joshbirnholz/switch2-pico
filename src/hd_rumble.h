#ifndef S2P_HD_RUMBLE_H
#define S2P_HD_RUMBLE_H

// HD rumble translation between the Switch 1 encoding (what a host sends to the
// emulated Pro Controller) and the Switch 2 encoding (what the Pro Controller 2
// linear resonant actuators accept over BLE).
//
// Pure C, no hardware dependencies; covered by test/test_hd_rumble.c.

#include <stdbool.h>
#include <stdint.h>

// One time-sample for one actuator.
typedef struct {
    float hi_freq_hz;
    float hi_amp;       // linear, 1.0 == the Switch 1 "max safe" amplitude
    float lo_freq_hz;
    float lo_amp;
} rumble_sample_t;

// Decoder state for one Switch 1 motor. The packed 5-bit commands are relative
// to the previous amplitude, so this state must persist between frames.
typedef struct {
    float hi_freq_hz;
    float lo_freq_hz;
    float hi_lvl;       // log2 amplitude relative to the 7-bit code 127 level
    float lo_lvl;
} s1_rumble_state_t;

void s1_rumble_reset(s1_rumble_state_t *st);

// Decode the 4 bytes for one motor. Writes 1..3 samples (in playback order)
// and returns how many were produced.
int s1_rumble_decode(s1_rumble_state_t *st, const uint8_t data[4], rumble_sample_t out[3]);

// Linear amplitude for a Switch 1 7-bit amplitude code (dekuNukem's curve).
float s1_amp_code_to_linear(int code);

// ---- Switch 2 ----
#define S2_RUMBLE_NEUTRAL_HI_FREQ 0x187   // what the console sends while idle
#define S2_RUMBLE_NEUTRAL_LO_FREQ 0x112
#define S2_RUMBLE_FRAME_LEN 5
#define S2_RUMBLE_BLOCK_LEN 16            // 0x50|seq + 3 frames
#define S2_RUMBLE_PRO_PACKET_LEN (1 + 2 * S2_RUMBLE_BLOCK_LEN)

void s2_rumble_encode_frame(uint16_t hi_freq, uint16_t hi_amp, uint16_t lo_freq, uint16_t lo_amp,
                            uint8_t out[S2_RUMBLE_FRAME_LEN]);

typedef struct {
    bool translate_freq;       // false: always use the neutral frequencies
    uint8_t freq_slope;        // Switch 2 frequency codes per octave
    uint8_t strength_pct;      // 0..200
} s2_rumble_params_t;

uint16_t s2_rumble_freq_code(float hz, bool is_hi, const s2_rumble_params_t *p);
uint16_t s2_rumble_amp_code(float linear, const s2_rumble_params_t *p);

// Encode one actuator block (header + 3 frames). `n` samples are spread over
// the 3 frame slots; the last one is repeated when fewer than 3 are given.
void s2_rumble_encode_block(uint8_t seq, const rumble_sample_t *samples, int n,
                            const s2_rumble_params_t *p, uint8_t out[S2_RUMBLE_BLOCK_LEN]);

// True if any frame in the block has non-zero amplitude.
bool s2_rumble_block_active(const uint8_t block[S2_RUMBLE_BLOCK_LEN]);

#endif
