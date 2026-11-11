// Minimal DHCPv4 server for the USB-NCM interface.
//
// Hands out <net>.2 .. <net>.9 (one lease per client MAC; the first host gets .2) and
// deliberately advertises NO router and NO DNS server, so plugging in the
// KB2040 never steals the host's default route or DNS resolver.
#pragma once

#include <lwip/netif.h>
#include <lwip/ip4_addr.h>

bool dhcpServerBegin(struct netif *nif, const ip4_addr_t *serverIp, const ip4_addr_t *netmask);
