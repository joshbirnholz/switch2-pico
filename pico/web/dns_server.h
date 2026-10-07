#ifndef S2P_DNS_SERVER_H
#define S2P_DNS_SERVER_H

#include "lwip/ip4_addr.h"

// Answers every DNS A query with our own address (captive portal).
void dns_server_start(const ip4_addr_t *ip);
void dns_server_stop(void);

#endif
