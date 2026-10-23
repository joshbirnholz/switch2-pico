#ifndef S2P_MIC_TEST_H
#define S2P_MIC_TEST_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "s2_proto.h"

typedef enum { MIC_TEST_IDLE, MIC_TEST_READY, MIC_TEST_PREPARING,
    MIC_TEST_RECORDING, MIC_TEST_PLAYBACK, MIC_TEST_DONE, MIC_TEST_ERROR } mic_test_phase_t;
typedef struct {
    uint32_t session, elapsed_ms, remaining_ms;
    uint16_t peak;
    mic_test_phase_t phase;
    const char *error;
} mic_test_status_t;
bool mic_test_supported(void);
bool mic_test_active(void);
bool mic_test_input(void);
uint32_t mic_test_open(uint32_t now);
bool mic_test_record(uint32_t session, uint32_t now);
bool mic_test_cancel(uint32_t session);
void mic_test_status(uint32_t session, uint32_t now, mic_test_status_t *out);
const char *mic_test_phase_name(mic_test_phase_t phase);
void mic_test_tick(uint32_t now, uint8_t jack, bool usb_busy);
// Main-loop producer; compressed audio never leaves dongle RAM.
void mic_test_frame(const uint8_t *frame, size_t len, uint32_t now);
// Core 1: one recorded frame per headphone encode, then publish its level.
bool mic_test_play_frame(uint8_t frame[S2_MIC_FRAME_BYTES], uint32_t *session, uint32_t now);
void mic_test_level(uint16_t peak, uint32_t session, uint32_t now);
void mic_test_fail(uint32_t session, const char *error);
#endif
