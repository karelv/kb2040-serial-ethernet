// Minimal DHCPv4 server (DISCOVER/OFFER, REQUEST/ACK|NAK, RELEASE) on lwIP raw UDP.
// Follows the same lwIP usage pattern as the core's WiFi/dhcpserver (arduino-pico wraps
// lwIP entry points with its own locking; the recv callback already runs in lwIP context).
#include <Arduino.h>
#include <string.h>
#include <lwip/udp.h>
#include <lwip/pbuf.h>
#include <lwip/ip_addr.h>
#include "dhcp_server.h"

namespace {

constexpr uint16_t kServerPort = 67;
constexpr uint16_t kClientPort = 68;
constexpr uint8_t kPoolStart = 2;   // .2 ..
constexpr uint8_t kPoolSize = 8;    // .. .9
constexpr uint32_t kLeaseSeconds = 24 * 3600;

enum : uint8_t { DISCOVER = 1, OFFER = 2, REQUEST = 3, DECLINE = 4, ACK = 5, NAK = 6, RELEASE = 7, INFORM = 8 };
enum : uint8_t {
  OPT_PAD = 0, OPT_SUBNET = 1, OPT_HOST_NAME = 12, OPT_REQ_IP = 50, OPT_LEASE = 51, OPT_MSG_TYPE = 53,
  OPT_SERVER_ID = 54, OPT_T1 = 58, OPT_T2 = 59, OPT_VENDOR_CLASS = 60, OPT_END = 255
};

struct __attribute__((packed)) DhcpMsg {
  uint8_t op, htype, hlen, hops;
  uint32_t xid;
  uint16_t secs, flags;
  uint8_t ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
  uint8_t chaddr[16];
  uint8_t sname[64];
  uint8_t file[128];
  uint8_t magic[4];
  uint8_t options[312];
};
constexpr size_t kHeaderLen = offsetof(DhcpMsg, options);  // 240
constexpr size_t kMinBootpLen = 300;
const uint8_t kMagic[4] = {99, 130, 83, 99};

struct Lease {
  uint8_t mac[6];
  bool used;
};

Lease g_leases[kPoolSize];
struct udp_pcb *g_pcb = nullptr;
struct netif *g_netif = nullptr;
uint8_t g_serverIp[4];
uint8_t g_netmask[4];
DhcpMsg g_rx, g_tx;  // static: callback runs in lwIP context, keep it off the stack
DhcpObserver g_observer = nullptr;

const uint8_t *findOpt(const uint8_t *opt, size_t len, uint8_t code) {
  size_t i = 0;
  while (i < len) {
    uint8_t c = opt[i];
    if (c == OPT_END) break;
    if (c == OPT_PAD) { i++; continue; }
    if (i + 1 >= len) break;
    uint8_t l = opt[i + 1];
    if (i + 2 + l > len) break;
    if (c == code) return &opt[i];
    i += 2 + l;
  }
  return nullptr;
}

void poolIp(int idx, uint8_t out[4]) {
  out[0] = g_serverIp[0];
  out[1] = g_serverIp[1];
  out[2] = g_serverIp[2];
  out[3] = kPoolStart + idx;
}

int findLease(const uint8_t *mac, bool allocate) {
  for (int i = 0; i < kPoolSize; i++) {
    if (g_leases[i].used && memcmp(g_leases[i].mac, mac, 6) == 0) return i;
  }
  if (!allocate) return -1;
  for (int i = 0; i < kPoolSize; i++) {
    if (!g_leases[i].used) {
      g_leases[i].used = true;
      memcpy(g_leases[i].mac, mac, 6);
      return i;
    }
  }
  return -1;
}

uint8_t *putOpt(uint8_t *p, uint8_t code, uint8_t len, const void *data) {
  *p++ = code;
  *p++ = len;
  memcpy(p, data, len);
  return p + len;
}

uint8_t *putU32(uint8_t *p, uint8_t code, uint32_t v) {
  uint8_t be[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  return putOpt(p, code, 4, be);
}

void sendReply(uint8_t type, int leaseIdx) {
  memset(&g_tx, 0, sizeof(g_tx));
  g_tx.op = 2;  // BOOTREPLY
  g_tx.htype = g_rx.htype;
  g_tx.hlen = g_rx.hlen;
  g_tx.xid = g_rx.xid;
  g_tx.flags = g_rx.flags;
  memcpy(g_tx.giaddr, g_rx.giaddr, 4);
  memcpy(g_tx.chaddr, g_rx.chaddr, sizeof(g_tx.chaddr));
  memcpy(g_tx.magic, kMagic, 4);
  if (type != NAK) {
    poolIp(leaseIdx, g_tx.yiaddr);
    memcpy(g_tx.siaddr, g_serverIp, 4);
  }

  uint8_t *p = g_tx.options;
  p = putOpt(p, OPT_MSG_TYPE, 1, &type);
  p = putOpt(p, OPT_SERVER_ID, 4, g_serverIp);
  if (type != NAK) {
    p = putOpt(p, OPT_SUBNET, 4, g_netmask);
    p = putU32(p, OPT_LEASE, kLeaseSeconds);
    p = putU32(p, OPT_T1, kLeaseSeconds / 2);
    p = putU32(p, OPT_T2, (kLeaseSeconds * 7) / 8);
    // No OPT_ROUTER / OPT_DNS on purpose: this is a point-to-point link only.
  }
  *p++ = OPT_END;

  size_t len = size_t(p - reinterpret_cast<uint8_t *>(&g_tx));
  if (len < kMinBootpLen) len = kMinBootpLen;

  struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
  if (!pb) return;
  memcpy(pb->payload, &g_tx, len);
  // Client has no IP yet: always broadcast on the (bound) USB interface.
  udp_sendto(g_pcb, pb, IP_ADDR_BROADCAST, kClientPort);
  pbuf_free(pb);
  if (g_observer) g_observer(type, false, g_tx.chaddr, nullptr, nullptr, g_tx.yiaddr[3]);
}

// Copies a string option (not NUL-terminated on the wire) into out; "" when absent.
void optString(size_t optLen, uint8_t code, char *out, size_t outLen) {
  out[0] = '\0';
  const uint8_t *o = findOpt(g_rx.options, optLen, code);
  if (!o) return;
  size_t n = o[1] < outLen - 1 ? o[1] : outLen - 1;
  memcpy(out, o + 2, n);
  out[n] = '\0';
}

void onDhcpPacket(void *, struct udp_pcb *, struct pbuf *p, const ip_addr_t *, u16_t) {
  memset(&g_rx, 0, sizeof(g_rx));
  size_t n = pbuf_copy_partial(p, &g_rx, sizeof(g_rx), 0);
  pbuf_free(p);

  if (n < kHeaderLen + 4 || g_rx.op != 1 || g_rx.hlen != 6 || memcmp(g_rx.magic, kMagic, 4) != 0) return;
  size_t optLen = n - kHeaderLen;

  const uint8_t *t = findOpt(g_rx.options, optLen, OPT_MSG_TYPE);
  if (!t || t[1] < 1) return;
  uint8_t type = t[2];
  if (g_observer) {
    char host[33], vendor[49];
    optString(optLen, OPT_HOST_NAME, host, sizeof(host));
    optString(optLen, OPT_VENDOR_CLASS, vendor, sizeof(vendor));
    g_observer(type, true, g_rx.chaddr, host, vendor, 0);
  }

  switch (type) {
    case DISCOVER: {
      int idx = findLease(g_rx.chaddr, true);
      if (idx >= 0) sendReply(OFFER, idx);
      break;
    }
    case REQUEST: {
      // Client selected a different server -> not for us.
      const uint8_t *sid = findOpt(g_rx.options, optLen, OPT_SERVER_ID);
      if (sid && sid[1] == 4 && memcmp(sid + 2, g_serverIp, 4) != 0) return;

      int idx = findLease(g_rx.chaddr, true);
      if (idx < 0) return;

      uint8_t want[4] = {0, 0, 0, 0};
      const uint8_t *req = findOpt(g_rx.options, optLen, OPT_REQ_IP);
      if (req && req[1] == 4) memcpy(want, req + 2, 4);
      else memcpy(want, g_rx.ciaddr, 4);

      uint8_t mine[4];
      poolIp(idx, mine);
      bool hasWant = want[0] | want[1] | want[2] | want[3];
      if (hasWant && memcmp(want, mine, 4) != 0) {
        sendReply(NAK, idx);  // stale address from a previous session -> restart DISCOVER
      } else {
        sendReply(ACK, idx);
      }
      break;
    }
    case RELEASE:
    case DECLINE: {
      int idx = findLease(g_rx.chaddr, false);
      if (idx >= 0) g_leases[idx].used = false;
      break;
    }
    default:
      break;
  }
}

}  // namespace

bool dhcpServerBegin(struct netif *nif, const ip4_addr_t *serverIp, const ip4_addr_t *netmask) {
  if (g_pcb) return true;
  memset(g_leases, 0, sizeof(g_leases));
  g_netif = nif;
  uint32_t ip = ip4_addr_get_u32(serverIp);    // network byte order in memory
  uint32_t nm = ip4_addr_get_u32(netmask);
  memcpy(g_serverIp, &ip, 4);
  memcpy(g_netmask, &nm, 4);

  g_pcb = udp_new();
  if (!g_pcb) return false;
  ip_set_option(g_pcb, SOF_BROADCAST);
  if (udp_bind(g_pcb, IP_ANY_TYPE, kServerPort) != ERR_OK) {
    udp_remove(g_pcb);
    g_pcb = nullptr;
    return false;
  }
  if (g_netif) udp_bind_netif(g_pcb, g_netif);
  udp_recv(g_pcb, onDhcpPacket, nullptr);
  return true;
}

void dhcpServerSetObserver(DhcpObserver fn) { g_observer = fn; }
