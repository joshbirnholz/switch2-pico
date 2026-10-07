#include "mcu_nfc.h"

#include <string.h>

#include "amiibo.h"

__attribute__((weak)) void mcu_hook_polling_changed(bool polling) { (void)polling; }
__attribute__((weak)) void mcu_hook_tag_written(void) {}

uint8_t mcu_crc8(const uint8_t *data, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// Packet builder: concatenates fragments, zero pads and appends the CRC.
typedef struct {
    uint8_t *buf;
    int pos;
} pkt_t;

static void pkt_begin(pkt_t *p, uint8_t *buf, uint8_t background) {
    p->buf = buf;
    p->pos = 0;
    memset(buf, background, MCU_PACKET_LEN);
}

static void pkt_bytes(pkt_t *p, const uint8_t *d, int n) {
    if (p->pos + n > MCU_PACKET_LEN - 1) n = MCU_PACKET_LEN - 1 - p->pos;
    if (n > 0) memcpy(p->buf + p->pos, d, (size_t)n);
    p->pos += n;
}

static void pkt_u8(pkt_t *p, uint8_t v) {
    pkt_bytes(p, &v, 1);
}

static void pkt_end(pkt_t *p) {
    p->buf[MCU_PACKET_LEN - 1] = mcu_crc8(p->buf, MCU_PACKET_LEN - 1);
}

static void queue_clear(mcu_t *m) {
    m->q_head = 0;
    m->q_count = 0;
}

static uint8_t *queue_slot(mcu_t *m, bool force) {
    if (m->q_count >= MCU_QUEUE_LEN) {
        if (!force) return NULL;
        // Drop the oldest so must-deliver packets (tag data) always fit.
        m->q_head = (m->q_head + 1) % MCU_QUEUE_LEN;
        m->q_count--;
    }
    int idx = (m->q_head + m->q_count) % MCU_QUEUE_LEN;
    m->q_count++;
    return m->queue[idx];
}

static void build_no_response(uint8_t *out) {
    pkt_t p;
    pkt_begin(&p, out, 0);
    pkt_u8(&p, 0xFF);
    pkt_end(&p);
}

static void queue_status(mcu_t *m) {
    if (m->power == MCU_SUSPENDED) return;
    uint8_t *out = queue_slot(m, false);
    if (!out) return;
    static const uint8_t hdr[] = {0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x1b};
    pkt_t p;
    pkt_begin(&p, out, 0);
    pkt_bytes(&p, hdr, sizeof hdr);
    pkt_u8(&p, (uint8_t)m->power);
    pkt_end(&p);
}

void mcu_reset(mcu_t *m) {
    memset(m, 0, sizeof *m);
    m->power = MCU_SUSPENDED;
    m->nfc_state = NFC_NONE;
}

void mcu_enter_report_mode_31(mcu_t *m) {
    queue_clear(m);
    if (m->power == MCU_SUSPENDED) m->power = MCU_READY;
    queue_status(m);
}

void mcu_set_power(mcu_t *m, uint8_t state) {
    queue_clear(m);
    m->power = state == MCU_SUSPENDED ? MCU_SUSPENDED : MCU_READY;
    if (m->power == MCU_SUSPENDED && m->polling) {
        m->polling = false;
        mcu_hook_polling_changed(false);
    }
    queue_status(m);
}

void mcu_set_config(mcu_t *m, const uint8_t *args, int len, uint8_t reply[MCU_CONFIG_REPLY_LEN]) {
    // args: [0x21][0x00][mode][...]
    uint8_t mode = len > 2 ? args[2] : 0;
    if (m->power != MCU_SUSPENDED) {
        if (mode == MCU_NFC) {
            m->power = MCU_NFC;
            m->nfc_state = NFC_NONE;
            m->counter = 0;
            m->ack_seq = 0;
            m->have_last_uid = false;
        } else {
            m->power = MCU_READY;
        }
        queue_status(m);
    }
    static const uint8_t base[8] = {0x01, 0x00, 0xFF, 0x00, 0x08, 0x00, 0x1B, 0x01};
    memset(reply, 0, MCU_CONFIG_REPLY_LEN);
    memcpy(reply, base, sizeof base);
    reply[MCU_CONFIG_REPLY_LEN - 1] = mcu_crc8(reply, MCU_CONFIG_REPLY_LEN - 1);
}

static bool tag_available(void) {
    return g_tag.state == TAG_READY;
}

static void build_nfc_status(mcu_t *m, uint8_t *out) {
    m->counter--;
    bool have_tag = tag_available();
    const uint8_t *uid = g_tag.uid;
    static const uint8_t zero_uid[7] = {0};

    // After a write the console expects the tag to disappear; report an
    // all-zero tag for a few polls.
    if ((m->nfc_state == NFC_POLL || m->nfc_state == NFC_POLL_AGAIN) && m->pending_remove > 0) {
        have_tag = true;
        uid = zero_uid;
        m->pending_remove--;
    }

    if (m->nfc_state == NFC_PROCESSING_WRITE && m->counter <= 0) {
        m->nfc_state = NFC_NONE;
    } else if (m->nfc_state == NFC_POLL) {
        if (have_tag && m->have_last_uid && memcmp(uid, m->last_uid, 7) == 0) {
            m->nfc_state = NFC_POLL_AGAIN;
        } else {
            m->have_last_uid = have_tag;
            if (have_tag) memcpy(m->last_uid, uid, 7);
        }
    } else if (m->nfc_state == NFC_POLL_AGAIN) {
        if (!have_tag || memcmp(uid, m->last_uid, 7) != 0) {
            m->nfc_state = NFC_POLL;
            m->have_last_uid = have_tag;
            if (have_tag) memcpy(m->last_uid, uid, 7);
        }
    }

    pkt_t p;
    pkt_begin(&p, out, 0);
    if (have_tag && m->nfc_state != NFC_NONE) {
        static const uint8_t h1[] = {0x2a, 0x00, 0x05};
        static const uint8_t h2[] = {0x09, 0x31};
        static const uint8_t h3[] = {0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x00, 0x07};
        pkt_bytes(&p, h1, sizeof h1);
        pkt_u8(&p, 0);            // our sequence number
        pkt_u8(&p, m->ack_seq);
        pkt_bytes(&p, h2, sizeof h2);
        pkt_u8(&p, (uint8_t)m->nfc_state);
        pkt_bytes(&p, h3, sizeof h3);
        pkt_bytes(&p, uid, 7);
    } else {
        static const uint8_t h[] = {0x2a, 0x00, 0x05, 0x00, 0x00, 0x09, 0x31};
        pkt_bytes(&p, h, sizeof h);
        pkt_u8(&p, (uint8_t)m->nfc_state);
    }
    pkt_end(&p);
}

static void queue_nfc_status(mcu_t *m, bool force) {
    uint8_t *out = queue_slot(m, force);
    if (out) build_nfc_status(m, out);
}

// Fixed part of the first "tag data" packet that precedes the page data.
static const uint8_t READ_HDR_A[] = {0x3a, 0x00, 0x07, 0x01, 0x00, 0x01, 0x31, 0x02,
                                     0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x07};
static const uint8_t READ_HDR_B[] = {
    0x00, 0x00, 0x00, 0x00, 0x7d, 0xfd, 0xf0, 0x79, 0x36, 0x51, 0xab, 0xd7, 0x46, 0x6e, 0x39,
    0xc1, 0x91, 0xba, 0xbe, 0xb8, 0x56, 0xce, 0xed, 0xf1, 0xce, 0x44, 0xcc, 0x75, 0xea, 0xfb,
    0x27, 0x09, 0x4d, 0x08, 0x7a, 0xe8, 0x03, 0x00, 0x3b, 0x3c, 0x77, 0x78, 0x86, 0x00, 0x00};
static const uint8_t READ_HDR_2[] = {0x3a, 0x00, 0x07, 0x02, 0x00, 0x09, 0x27};
static const uint8_t READ_TRAILER[] = {0x2a, 0x00, 0x05, 0x00, 0x00, 0x09, 0x31, 0x04,
                                       0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x00, 0x07};
static const uint8_t WRITE_HDR_A[] = {0x3a, 0x00, 0x07, 0x01, 0x00, 0x08, 0x40, 0x02,
                                      0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x07};
static const uint8_t WRITE_HDR_B[] = {
    0x00, 0x00, 0x00, 0x00, 0xfd, 0xb0, 0xc0, 0xa4, 0x34, 0xc9, 0xbf, 0x31, 0x69, 0x00, 0x30, 0xaa, 0xef,
    0x56, 0x44, 0x4b, 0x0f, 0x60, 0x26, 0x27, 0x36, 0x6d, 0x5a, 0x28, 0x1a, 0xdc, 0x69, 0x7f, 0xde, 0x0d,
    0x6c, 0xbc, 0x01, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf1, 0x10, 0xff, 0xee};

static void queue_tag_read(mcu_t *m) {
    pkt_t p;
    uint8_t *out;

    queue_clear(m);
    out = queue_slot(m, true);
    pkt_begin(&p, out, 0);
    pkt_bytes(&p, READ_HDR_A, sizeof READ_HDR_A);
    pkt_bytes(&p, g_tag.uid, 7);
    pkt_bytes(&p, READ_HDR_B, sizeof READ_HDR_B);
    pkt_bytes(&p, g_tag.data, 245);
    pkt_end(&p);

    out = queue_slot(m, true);
    pkt_begin(&p, out, 0);
    pkt_bytes(&p, READ_HDR_2, sizeof READ_HDR_2);
    pkt_bytes(&p, g_tag.data + 245, NTAG215_SIZE - 245);
    pkt_end(&p);

    out = queue_slot(m, true);
    pkt_begin(&p, out, 0);
    pkt_bytes(&p, READ_TRAILER, sizeof READ_TRAILER);
    pkt_bytes(&p, g_tag.uid, 7);
    pkt_end(&p);
}

static void queue_write_setup(mcu_t *m) {
    pkt_t p;
    uint8_t *out = queue_slot(m, true);
    pkt_begin(&p, out, 0);
    pkt_bytes(&p, WRITE_HDR_A, sizeof WRITE_HDR_A);
    pkt_bytes(&p, g_tag.uid, 7);
    pkt_bytes(&p, WRITE_HDR_B, sizeof WRITE_HDR_B);
    pkt_end(&p);
}

// Apply an NTAG write command assembled from the host's 0x08 packets to our
// cached copy of the tag. Layout (joycontrol): [1] uid length, [2..8] uid,
// [13..16] page 4 while writing, [17..20] page 4 after writing, then from 22
// a list of (page, length, data) records terminated by a zero page/length.
static void apply_write(const uint8_t *cmd, int len) {
    if (len < 22 || cmd[1] != 7 || g_tag.state != TAG_READY) return;
    int i = 22;
    while (i + 1 < len) {
        int addr = cmd[i] * 4;
        int n = cmd[i + 1];
        if (addr == 0 || n == 0) break;
        if (i + 2 + n > len) n = len - (i + 2);
        if (addr + n > NTAG215_SIZE) n = NTAG215_SIZE - addr;
        if (n <= 0) break;
        memcpy(g_tag.data + addr, cmd + i + 2, (size_t)n);
        i += 2 + cmd[i + 1];
    }
    memcpy(g_tag.data + 16, cmd + 17, 4);
    g_tag.modified_by_host = true;
    mcu_hook_tag_written();
}

static void handle_nfc_command(mcu_t *m, uint8_t com, const uint8_t *d, int len) {
    switch (com) {
    case 0x04:   // status request (the host sends these continuously)
        queue_nfc_status(m, false);
        break;
    case 0x01:   // start polling
        m->nfc_state = NFC_POLL;
        if (!m->polling) {
            m->polling = true;
            mcu_hook_polling_changed(true);
        }
        break;
    case 0x02:   // stop polling
        m->nfc_state = NFC_NONE;
        m->have_last_uid = false;
        if (m->polling) {
            m->polling = false;
            mcu_hook_polling_changed(false);
        }
        break;
    case 0x06: { // read, or prepare to write when a UID is given
        if (!tag_available()) break;
        bool any_uid = true;
        for (int i = 6; i < 13 && i < len; i++) {
            if (d[i] != 0) any_uid = false;
        }
        if (any_uid) {
            queue_tag_read(m);
        } else {
            queue_write_setup(m);
            m->write_len = 0;
            m->nfc_state = NFC_AWAITING_WRITE;
        }
        break;
    }
    case 0x08: { // write data: [seq][?][flags][len][data...]
        if (len < 4) break;
        uint8_t seq = d[0];
        int n = d[3];
        if (4 + n > len) n = len - 4;
        if (seq == (uint8_t)(m->ack_seq + 1)) {
            if (m->write_len + n <= MCU_WRITE_BUF) {
                memcpy(m->write_buf + m->write_len, d + 4, (size_t)n);
                m->write_len += n;
            }
            m->ack_seq++;
        } else if (seq > m->ack_seq + 1) {
            m->ack_seq = 0;
            m->write_len = 0;
        }
        m->nfc_state = NFC_WRITING;
        queue_nfc_status(m, true);
        if (d[2] == 0x08) {   // last packet
            m->ack_seq = 0;
            m->nfc_state = NFC_PROCESSING_WRITE;
            m->counter = 4;
            m->pending_remove = 4;
            apply_write(m->write_buf, m->write_len);
            m->write_len = 0;
        }
        break;
    }
    default:
        break;
    }
}

void mcu_handle_request(mcu_t *m, const uint8_t *payload, int len) {
    if (len < 10) return;
    uint8_t sub = payload[9];
    const uint8_t *d = payload + 10;
    int dlen = len - 10;
    if (sub == 0x01) {
        queue_status(m);
    } else if (sub == 0x02 && m->power == MCU_NFC && dlen >= 1) {
        handle_nfc_command(m, d[0], d + 1, dlen - 1);
    }
}

void mcu_next_packet(mcu_t *m, uint8_t out[MCU_PACKET_LEN]) {
    if (m->q_count > 0) {
        memcpy(out, m->queue[m->q_head], MCU_PACKET_LEN);
        m->q_head = (m->q_head + 1) % MCU_QUEUE_LEN;
        m->q_count--;
    } else {
        build_no_response(out);
    }
}
