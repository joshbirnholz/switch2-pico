#include "amiibo.h"

#include <string.h>

amiibo_tag_t g_tag;

void amiibo_clear(void) {
    g_tag.state = TAG_NONE;
    memset(g_tag.uid, 0, sizeof g_tag.uid);
}

void amiibo_set_uid(const uint8_t uid[NFC_UID_LEN]) {
    if (g_tag.state != TAG_NONE && memcmp(g_tag.uid, uid, NFC_UID_LEN) == 0) {
        return;
    }
    memcpy(g_tag.uid, uid, NFC_UID_LEN);
    g_tag.state = TAG_READING;
    g_tag.modified_by_host = false;
}

void amiibo_set_data(const uint8_t data[NTAG215_SIZE]) {
    memcpy(g_tag.data, data, NTAG215_SIZE);
    g_tag.state = TAG_READY;
    g_tag.generation++;
}

void amiibo_set_failed(void) {
    if (g_tag.state == TAG_READING) g_tag.state = TAG_READ_FAILED;
}

int amiibo_find_pages(const uint8_t *buf, size_t len, const uint8_t uid[NFC_UID_LEN]) {
    // NTAG page 0: UID0 UID1 UID2 BCC0, page 1: UID3..UID6, page 2: BCC1 ...
    uint8_t bcc0 = (uint8_t)(0x88 ^ uid[0] ^ uid[1] ^ uid[2]);
    uint8_t bcc1 = (uint8_t)(uid[3] ^ uid[4] ^ uid[5] ^ uid[6]);
    for (size_t i = 0; i + 9 <= len; i++) {
        if (buf[i] == uid[0] && buf[i + 1] == uid[1] && buf[i + 2] == uid[2] && buf[i + 3] == bcc0 &&
            memcmp(buf + i + 4, uid + 3, 4) == 0 && buf[i + 8] == bcc1) {
            return (int)i;
        }
    }
    return -1;
}

bool amiibo_get_id(uint8_t out[8]) {
    if (g_tag.state != TAG_READY) return false;
    memcpy(out, g_tag.data + 0x54, 8);
    return true;
}
