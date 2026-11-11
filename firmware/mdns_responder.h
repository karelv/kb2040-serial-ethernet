// Minimal mDNS (RFC 6762) responder for the USB-NCM interface.
//
// Answers A queries for up to kMdnsMaxNames host names (e.g. "kb2040-1" and the
// alias "kb2040") with the board's IPv4 address, and answers AAAA queries with an
// NSEC "no IPv6" record so resolvers don't wait for an AAAA timeout.
// lwIP's own mdns app supports only one host name per netif, hence this module.
#pragma once

#include <lwip/netif.h>
#include <lwip/ip4_addr.h>

constexpr int kMdnsMaxNames = 2;

// names[0] is the board's unique name (answered with the cache-flush bit set);
// further names are shared aliases (answered without cache-flush, so two boards on
// two interfaces may both answer them).
bool mdnsBegin(struct netif *nif, const ip4_addr_t *ip, const char *const *names, int count);
