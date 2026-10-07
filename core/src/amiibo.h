#ifndef S2P_AMIIBO_H
#define S2P_AMIIBO_H

#ifdef __cplusplus
extern "C" {
#endif

// The most recently scanned NFC tag (NTAG215 / amiibo), shared between the
// Switch 2 reader (s2_link.c) and the emulated Switch 1 NFC MCU (mcu_nfc.c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NTAG215_SIZE 540
#define NFC_UID_LEN 7

typedef enum {
    TAG_NONE = 0,       // nothing on the reader
    TAG_READING,        // UID seen, page data still being transferred
    TAG_READY,          // UID + full page data available
    TAG_READ_FAILED,    // UID seen but the read failed (UID still usable)
} tag_state_t;

typedef struct {
    tag_state_t state;
    uint8_t uid[NFC_UID_LEN];
    uint8_t data[NTAG215_SIZE];
    uint32_t generation;        // incremented whenever a new tag is read
    bool modified_by_host;      // the host wrote to our cached copy
    uint32_t last_seen_ms;
} amiibo_tag_t;

extern amiibo_tag_t g_tag;

void amiibo_clear(void);
void amiibo_set_uid(const uint8_t uid[NFC_UID_LEN]);
void amiibo_set_data(const uint8_t data[NTAG215_SIZE]);
void amiibo_set_failed(void);

// Find the start of NTAG page data inside an arbitrary buffer by matching the
// UID and block check characters of pages 0..2. Returns -1 if not found.
int amiibo_find_pages(const uint8_t *buf, size_t len, const uint8_t uid[NFC_UID_LEN]);

// The 8 byte amiibo identification block (character/variant/type/model/series)
// stored unencrypted at pages 21-22. Returns false if no data is available.
bool amiibo_get_id(uint8_t out[8]);

#ifdef __cplusplus
}
#endif

#endif
