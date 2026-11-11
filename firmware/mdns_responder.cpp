// Minimal mDNS responder on lwIP raw UDP (same usage pattern as dhcp_server.cpp: the recv
// callback runs in lwIP context; arduino-pico wraps udp_*/igmp_* calls with its lwIP lock).
#include <Arduino.h>
#include <string.h>
#include <ctype.h>
#include <lwip/udp.h>
#include <lwip/igmp.h>
#include <lwip/pbuf.h>
#include <lwip/ip_addr.h>
#include "mdns_responder.h"

namespace {

constexpr uint16_t kMdnsPort = 5353;
constexpr uint16_t TYPE_A = 1, TYPE_AAAA = 28, TYPE_NSEC = 47, TYPE_ANY = 255;
constexpr uint32_t kTtl = 120;
constexpr size_t kMaxPacket = 512;
constexpr size_t kMaxName = 64;

struct udp_pcb *g_pcb = nullptr;
struct netif *g_netif = nullptr;
uint8_t g_ip[4];
char g_names[kMdnsMaxNames][kMaxName];  // "kb2040-1.local", lower case
int g_nameCount = 0;
uint8_t g_rx[kMaxPacket];
uint8_t g_tx[kMaxPacket];

uint16_t rd16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
uint8_t *wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; return p + 2; }
uint8_t *wr32(uint8_t *p, uint32_t v) { p = wr16(p, v >> 16); return wr16(p, v & 0xffff); }

// Decodes a (possibly compressed) DNS name at *off into dotted lower-case form.
// Advances *off past the name in the original position. Returns false if malformed.
bool readName(const uint8_t *msg, size_t len, size_t *off, char *out, size_t outLen) {
  size_t pos = *off, o = 0;
  bool jumped = false;
  int hops = 0;
  while (true) {
    if (pos >= len) return false;
    uint8_t l = msg[pos];
    if ((l & 0xC0) == 0xC0) {
      if (pos + 1 >= len || ++hops > 8) return false;
      if (!jumped) *off = pos + 2;
      pos = ((l & 0x3F) << 8) | msg[pos + 1];
      jumped = true;
      continue;
    }
    if (l == 0) {
      if (!jumped) *off = pos + 1;
      break;
    }
    if (pos + 1 + l > len) return false;
    if (o && o + 1 < outLen) out[o++] = '.';
    for (uint8_t i = 0; i < l && o + 1 < outLen; i++) out[o++] = (char)tolower(msg[pos + 1 + i]);
    pos += 1 + l;
  }
  out[o] = '\0';
  return true;
}

uint8_t *writeName(uint8_t *p, const char *name) {
  while (*name) {
    const char *dot = strchr(name, '.');
    size_t l = dot ? size_t(dot - name) : strlen(name);
    *p++ = (uint8_t)l;
    memcpy(p, name, l);
    p += l;
    name += l;
    if (*name == '.') name++;
  }
  *p++ = 0;
  return p;
}

uint8_t *writeA(uint8_t *p, int nameIdx, uint32_t ttl) {
  p = writeName(p, g_names[nameIdx]);
  p = wr16(p, TYPE_A);
  p = wr16(p, nameIdx == 0 ? 0x8001 : 0x0001);  // cache-flush only for the unique name
  p = wr32(p, ttl);
  p = wr16(p, 4);
  memcpy(p, g_ip, 4);
  return p + 4;
}

// NSEC asserting that only an A record exists for this name (RFC 6762 section 6.1).
uint8_t *writeNsec(uint8_t *p, int nameIdx, uint32_t ttl) {
  p = writeName(p, g_names[nameIdx]);
  p = wr16(p, TYPE_NSEC);
  p = wr16(p, nameIdx == 0 ? 0x8001 : 0x0001);
  p = wr32(p, ttl);
  uint8_t *rdlen = p;
  p += 2;
  uint8_t *start = p;
  p = writeName(p, g_names[nameIdx]);
  *p++ = 0;     // window block 0
  *p++ = 1;     // bitmap length
  *p++ = 0x40;  // bit 1 = type A
  wr16(rdlen, uint16_t(p - start));
  return p;
}

int matchName(const char *n) {
  for (int i = 0; i < g_nameCount; i++)
    if (strcmp(n, g_names[i]) == 0) return i;
  return -1;
}

void onMdnsPacket(void *, struct udp_pcb *, struct pbuf *p, const ip_addr_t *src, u16_t srcPort) {
  size_t len = pbuf_copy_partial(p, g_rx, sizeof(g_rx), 0);
  pbuf_free(p);
  if (len < 12) return;

  uint16_t id = rd16(g_rx), flags = rd16(g_rx + 2), qd = rd16(g_rx + 4);
  if (flags & 0x8000) return;           // a response, not a query
  if ((flags & 0x7800) != 0) return;    // opcode != QUERY
  bool legacy = srcPort != kMdnsPort;   // one-shot resolver: unicast reply, echo the question

  bool wantA[kMdnsMaxNames] = {}, wantNsec[kMdnsMaxNames] = {};
  int legacyQ = -1;
  uint16_t legacyType = 0;
  size_t off = 12;
  char name[kMaxName];
  for (uint16_t q = 0; q < qd && q < 16; q++) {
    if (!readName(g_rx, len, &off, name, sizeof(name)) || off + 4 > len) return;
    uint16_t qtype = rd16(g_rx + off);
    off += 4;  // type + class (QU bit ignored: multicast answers are always acceptable)
    int idx = matchName(name);
    if (idx < 0) continue;
    if (qtype == TYPE_A || qtype == TYPE_ANY) wantA[idx] = true;
    else if (qtype == TYPE_AAAA) wantNsec[idx] = true;
    else continue;
    if (legacyQ < 0) { legacyQ = idx; legacyType = qtype; }
  }

  uint16_t an = 0, ar = 0;
  for (int i = 0; i < g_nameCount; i++) { an += wantA[i]; ar += wantNsec[i]; }
  if (!an && !ar) return;

  uint32_t ttl = legacy ? 10 : kTtl;
  uint8_t *w = g_tx;
  w = wr16(w, legacy ? id : 0);
  w = wr16(w, 0x8400);  // QR + AA
  w = wr16(w, legacy ? 1 : 0);
  w = wr16(w, an);
  w = wr16(w, 0);
  w = wr16(w, ar);
  if (legacy) {
    w = writeName(w, g_names[legacyQ]);
    w = wr16(w, legacyType);
    w = wr16(w, 1);
  }
  for (int i = 0; i < g_nameCount; i++) if (wantA[i]) w = writeA(w, i, ttl);
  for (int i = 0; i < g_nameCount; i++) if (wantNsec[i]) w = writeNsec(w, i, ttl);

  size_t outLen = size_t(w - g_tx);
  struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, outLen, PBUF_RAM);
  if (!pb) return;
  memcpy(pb->payload, g_tx, outLen);
  if (legacy) {
    udp_sendto_if(g_pcb, pb, src, srcPort, g_netif);
  } else {
    ip_addr_t group;
    IP_ADDR4(&group, 224, 0, 0, 251);
    udp_sendto_if(g_pcb, pb, &group, kMdnsPort, g_netif);
  }
  pbuf_free(pb);
}

}  // namespace

bool mdnsBegin(struct netif *nif, const ip4_addr_t *ip, const char *const *names, int count) {
  if (g_pcb) return true;
  g_netif = nif;
  uint32_t a = ip4_addr_get_u32(ip);
  memcpy(g_ip, &a, 4);
  g_nameCount = 0;
  for (int i = 0; i < count && i < kMdnsMaxNames; i++) {
    snprintf(g_names[i], kMaxName, "%s.local", names[i]);
    for (char *c = g_names[i]; *c; c++) *c = (char)tolower(*c);
    g_nameCount++;
  }

  ip4_addr_t group;
  IP4_ADDR(&group, 224, 0, 0, 251);
  if (igmp_joingroup_netif(nif, &group) != ERR_OK) return false;

  g_pcb = udp_new();
  if (!g_pcb) return false;
  ip_set_option(g_pcb, SOF_REUSEADDR);
  if (udp_bind(g_pcb, IP_ANY_TYPE, kMdnsPort) != ERR_OK) {
    udp_remove(g_pcb);
    g_pcb = nullptr;
    return false;
  }
  udp_bind_netif(g_pcb, nif);
  udp_set_multicast_ttl(g_pcb, 255);
  udp_recv(g_pcb, onMdnsPacket, nullptr);
  return true;
}
