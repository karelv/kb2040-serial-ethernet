// Minimal DHCPv4 server for the USB-NCM interface.
//
// Hands out <net>.2 .. <net>.9 (one lease per client MAC; the first host gets .2) and
// deliberately advertises NO router and NO DNS server, so plugging in the
// KB2040 never steals the host's default route or DNS resolver.
#pragma once

#include <lwip/netif.h>
#include <lwip/ip4_addr.h>

bool dhcpServerBegin(struct netif *nif, const ip4_addr_t *serverIp, const ip4_addr_t *netmask);

// Optional diagnostics hook: called for every DHCP message received (rx = true; hostName and
// vendorClass from options 12 / 60, may be empty) and sent (leasedOctet = last byte of yiaddr).
typedef void (*DhcpObserver)(uint8_t msgType, bool rx, const uint8_t *mac, const char *hostName,
                             const char *vendorClass, uint8_t leasedOctet);
void dhcpServerSetObserver(DhcpObserver fn);
