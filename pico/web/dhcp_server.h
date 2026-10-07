#ifndef S2P_DHCP_SERVER_H
#define S2P_DHCP_SERVER_H

#include "lwip/ip4_addr.h"

// Tiny DHCP server for the configuration access point.
void dhcp_server_start(const ip4_addr_t *ip, const ip4_addr_t *mask);
void dhcp_server_stop(void);

#endif
