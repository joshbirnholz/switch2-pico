#include "dns_server.h"

#include <string.h>

#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define DNS_PORT 53
#define DNS_MAX 512

static struct udp_pcb *s_pcb;
static ip4_addr_t s_ip;

static void dns_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg;
    static uint8_t buf[DNS_MAX];
    if (!p) return;
    size_t len = pbuf_copy_partial(p, buf, sizeof buf - 16, 0);
    pbuf_free(p);
    if (len < 12) return;
    if (buf[2] & 0x80) return;                  // not a query
    uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
    if (qd != 1) return;

    // Walk the question name.
    size_t i = 12;
    while (i < len && buf[i] != 0) i += 1u + buf[i];
    i++;                                        // terminating zero
    if (i + 4 > len) return;
    uint16_t qtype = (uint16_t)((buf[i] << 8) | buf[i + 1]);
    size_t qend = i + 4;

    buf[2] = 0x84 | (buf[2] & 0x01);            // response, authoritative, keep RD
    buf[3] = 0x00;
    buf[6] = 0x00;
    buf[7] = qtype == 1 ? 1 : 0;                // one answer for A queries
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    size_t out = qend;
    if (qtype == 1) {
        static const uint8_t ans[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04};
        memcpy(buf + out, ans, sizeof ans);
        out += sizeof ans;
        memcpy(buf + out, &s_ip.addr, 4);
        out += 4;
    }
    struct pbuf *r = pbuf_alloc(PBUF_TRANSPORT, (u16_t)out, PBUF_RAM);
    if (!r) return;
    memcpy(r->payload, buf, out);
    udp_sendto(pcb, r, addr, port);
    pbuf_free(r);
}

void dns_server_start(const ip4_addr_t *ip) {
    if (s_pcb) return;
    s_ip = *ip;
    s_pcb = udp_new();
    if (!s_pcb) return;
    udp_bind(s_pcb, IP_ANY_TYPE, DNS_PORT);
    udp_recv(s_pcb, dns_recv, NULL);
}

void dns_server_stop(void) {
    if (s_pcb) {
        udp_remove(s_pcb);
        s_pcb = NULL;
    }
}
