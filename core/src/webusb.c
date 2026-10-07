#include "webusb.h"

#include <stdlib.h>
#include <string.h>

#include "tusb.h"

#include "log.h"
#include "settings.h"
#include "web_api.h"

#define REQ_MAX 6144

static uint8_t s_rx[REQ_MAX + 1];
static uint32_t s_rx_len;
static uint32_t s_req_len;            // payload length of the request being received

static http_response_t s_resp;
static uint8_t s_hdr[12];
static uint32_t s_hdr_sent;
static uint32_t s_body_sent;
static bool s_sending;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void finish_response(void) {
    if (s_resp.body_owned && s_resp.body) free((void *)s_resp.body);
    memset(&s_resp, 0, sizeof s_resp);
    s_sending = false;
}

static void reset(void) {
    s_rx_len = 0;
    s_req_len = 0;
    if (s_sending) finish_response();
}

static void handle_request(void) {
    char *req = (char *)s_rx + 8;
    req[s_req_len] = 0;
    char *nl = strchr(req, '\n');
    char *body = nl ? nl + 1 : req + s_req_len;
    if (nl) *nl = 0;

    http_request_t r;
    memset(&r, 0, sizeof r);
    char *sp = strchr(req, ' ');
    memset(&s_resp, 0, sizeof s_resp);
    if (!sp) {
        s_resp.status = 400;
    } else {
        *sp = 0;
        r.method = req;
        r.path = sp + 1;
        char *q = strchr(sp + 1, '?');
        if (q) {
            *q = 0;
            r.query = q + 1;
        } else {
            r.query = "";
        }
        r.body = body;
        r.body_len = (size_t)(req + s_req_len - body);
        web_api_handle(&r, &s_resp);
    }
    uint32_t len = (uint32_t)s_resp.body_len;
    uint8_t h[12] = {'S', '2', 'P', 'R', (uint8_t)s_resp.status, (uint8_t)(s_resp.status >> 8), 0, 0,
                     (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
    memcpy(s_hdr, h, sizeof h);
    s_hdr_sent = 0;
    s_body_sent = 0;
    s_sending = true;
}

static void pump_tx(void) {
    while (s_sending) {
        uint32_t room = tud_vendor_write_available();
        if (!room) break;
        uint32_t n;
        if (s_hdr_sent < sizeof s_hdr) {
            n = tud_vendor_write(s_hdr + s_hdr_sent, tu_min32(room, sizeof s_hdr - s_hdr_sent));
            s_hdr_sent += n;
        } else if (s_body_sent < s_resp.body_len) {
            n = tud_vendor_write(s_resp.body + s_body_sent, tu_min32(room, (uint32_t)(s_resp.body_len - s_body_sent)));
            s_body_sent += n;
        } else {
            finish_response();
            n = 1;
        }
        if (!n) break;
    }
    tud_vendor_write_flush();
}

void webusb_task(void) {
    if (!tud_vendor_mounted()) {
        if (s_rx_len || s_sending) reset();
        return;
    }
    if (s_sending) {
        pump_tx();
        return;
    }
    while (tud_vendor_available()) {
        uint32_t want = s_req_len ? 8 + s_req_len - s_rx_len : 8 - s_rx_len;
        uint32_t n = tud_vendor_read(s_rx + s_rx_len, want);
        if (!n) break;
        s_rx_len += n;
        if (!s_req_len && s_rx_len == 8) {
            uint32_t len = rd32(s_rx + 4);
            if (memcmp(s_rx, "S2PQ", 4) != 0 || len == 0 || len > REQ_MAX - 8) {
                // Out of sync or oversized: drop everything buffered and start over.
                LOG("webusb: bad request header");
                tud_vendor_read_flush();
                s_rx_len = 0;
                return;
            }
            s_req_len = len;
        }
        if (s_req_len && s_rx_len == 8 + s_req_len) {
            handle_request();
            s_rx_len = 0;
            s_req_len = 0;
            pump_tx();
            return;
        }
    }
}

