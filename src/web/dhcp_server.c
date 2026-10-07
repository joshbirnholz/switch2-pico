#include "dhcp_server.h"

#include <string.h>

#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "pico/time.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAX_LEASES 8
#define DHCP_FIRST_HOST 16
#define DHCP_LEASE_SECONDS (24 * 60 * 60)

#define DHCP_OP_REQUEST 1
#define DHCP_OP_REPLY 2
#define DHCP_MAGIC 0x63825363u

#define OPT_PAD 0
#define OPT_SUBNET_MASK 1
#define OPT_ROUTER 3
#define OPT_DNS 6
#define OPT_REQUESTED_IP 50
#define OPT_LEASE_TIME 51
#define OPT_MSG_TYPE 53
#define OPT_SERVER_ID 54
#define OPT_END 255

#define DHCPDISCOVER 1
#define DHCPOFFER 2
#define DHCPREQUEST 3
#define DHCPACK 5
#define DHCPNAK 6
#define DHCPRELEASE 7

typedef struct __attribute__((packed)) {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint8_t ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t magic[4];
    uint8_t options[312];
} dhcp_msg_t;

typedef struct {
    uint8_t mac[6];
    uint32_t expiry_ms;
    bool used;
} lease_t;

static struct udp_pcb *s_pcb;
static ip4_addr_t s_ip, s_mask;
static lease_t s_leases[DHCP_MAX_LEASES];

static const uint8_t *find_option(const uint8_t *opt, size_t len, uint8_t code) {
    size_t i = 0;
    while (i < len && opt[i] != OPT_END) {
        if (opt[i] == OPT_PAD) {
            i++;
            continue;
        }
        if (i + 1 >= len) break;
        if (opt[i] == code) return &opt[i];
        i += 2u + opt[i + 1];
    }
    return NULL;
}

static uint8_t *put_opt(uint8_t *p, uint8_t code, const void *data, uint8_t n) {
    *p++ = code;
    *p++ = n;
    memcpy(p, data, n);
    return p + n;
}

static int lease_for(const uint8_t *mac) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    int free_slot = -1;
    for (int i = 0; i < DHCP_MAX_LEASES; i++) {
        if (s_leases[i].used && memcmp(s_leases[i].mac, mac, 6) == 0) return i;
        if (free_slot < 0 && (!s_leases[i].used || (int32_t)(now - s_leases[i].expiry_ms) > 0)) free_slot = i;
    }
    return free_slot;
}

static void send_reply(struct netif *nif, dhcp_msg_t *m, uint8_t type, int lease) {
    m->op = DHCP_OP_REPLY;
    memset(m->yiaddr, 0, 4);
    if (lease >= 0) {
        memcpy(m->yiaddr, &s_ip.addr, 4);
        m->yiaddr[3] = (uint8_t)(DHCP_FIRST_HOST + lease);
    }
    uint8_t *p = m->options;
    p = put_opt(p, OPT_MSG_TYPE, &type, 1);
    p = put_opt(p, OPT_SERVER_ID, &s_ip.addr, 4);
    p = put_opt(p, OPT_SUBNET_MASK, &s_mask.addr, 4);
    p = put_opt(p, OPT_ROUTER, &s_ip.addr, 4);
    p = put_opt(p, OPT_DNS, &s_ip.addr, 4);
    uint32_t lt = lwip_htonl(DHCP_LEASE_SECONDS);
    p = put_opt(p, OPT_LEASE_TIME, &lt, 4);
    *p++ = OPT_END;

    size_t len = (size_t)(p - (uint8_t *)m);
    struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!pb) return;
    memcpy(pb->payload, m, len);
    ip_addr_t dest;
    IP4_ADDR(ip_2_ip4(&dest), 255, 255, 255, 255);
    udp_sendto_if(s_pcb, pb, &dest, DHCP_CLIENT_PORT, nif);
    pbuf_free(pb);
}

static void dhcp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg;
    (void)pcb;
    (void)addr;
    (void)port;
    static dhcp_msg_t m;
    if (!p) return;
    memset(&m, 0, sizeof m);
    size_t len = pbuf_copy_partial(p, &m, sizeof m, 0);
    struct netif *nif = ip_current_input_netif();
    pbuf_free(p);
    if (len < offsetof(dhcp_msg_t, options) + 3 || m.op != DHCP_OP_REQUEST) return;

    size_t opt_len = len - offsetof(dhcp_msg_t, options);
    const uint8_t *mt = find_option(m.options, opt_len, OPT_MSG_TYPE);
    if (!mt) return;
    uint8_t type = mt[2];
    int lease = lease_for(m.chaddr);
    uint32_t now = to_ms_since_boot(get_absolute_time());

    switch (type) {
    case DHCPDISCOVER:
        if (lease < 0) return;
        send_reply(nif, &m, DHCPOFFER, lease);
        break;
    case DHCPREQUEST: {
        if (lease < 0) return;
        const uint8_t *req = find_option(m.options, opt_len, OPT_REQUESTED_IP);
        uint8_t want[4];
        memcpy(want, &s_ip.addr, 4);
        want[3] = (uint8_t)(DHCP_FIRST_HOST + lease);
        if (req && memcmp(req + 2, want, 4) != 0) {
            send_reply(nif, &m, DHCPNAK, -1);
            return;
        }
        memcpy(s_leases[lease].mac, m.chaddr, 6);
        s_leases[lease].used = true;
        s_leases[lease].expiry_ms = now + DHCP_LEASE_SECONDS * 1000u;
        send_reply(nif, &m, DHCPACK, lease);
        break;
    }
    case DHCPRELEASE:
        if (lease >= 0) s_leases[lease].used = false;
        break;
    default:
        break;
    }
}

void dhcp_server_start(const ip4_addr_t *ip, const ip4_addr_t *mask) {
    if (s_pcb) return;
    s_ip = *ip;
    s_mask = *mask;
    memset(s_leases, 0, sizeof s_leases);
    s_pcb = udp_new();
    if (!s_pcb) return;
    ip_set_option(s_pcb, SOF_BROADCAST);
    udp_bind(s_pcb, IP_ANY_TYPE, DHCP_SERVER_PORT);
    udp_recv(s_pcb, dhcp_recv, NULL);
}

void dhcp_server_stop(void) {
    if (s_pcb) {
        udp_remove(s_pcb);
        s_pcb = NULL;
    }
}
