// Flight recorder, see flight_log.h.
//
// One fixed-size record per boot in /flightN.bin (N = 0..kSlots-1, ring by sequence number).
// Saving: within a second of a milestone (first USB mount, first frame, DHCP step, first HTTP
// request, ...), every 10 s during the first 10 minutes (so the record also shows how long the
// board stayed powered), and every 5 minutes after that if counters changed. Nothing is written
// while an OTA update is in progress.
//
// USB endpoints: the RP2040 enables an endpoint in hardware when TinyUSB opens it. The CDC-NCM
// data endpoints are only opened when the host selects the data interface's alternate setting 1,
// i.e. when the host's network driver has actually started. Comparing the enabled endpoints at
// mount time with the maximum seen afterwards shows whether (and when) that happened.
#include "flight_log.h"

#include <LittleFS.h>
#include <tusb.h>
#include <hardware/structs/usb.h>

namespace {

constexpr uint32_t kMagic = 0x464C5401;  // "FLT" + record layout version
constexpr int kSlots = 8;
constexpr uint32_t kFastWindowMs = 10UL * 60 * 1000;
constexpr uint32_t kFastPeriodMs = 10000, kSlowPeriodMs = 300000, kMilestoneDelayMs = 1000;

struct Record {
  uint32_t magic, seq;
  char version[32];
  uint32_t savedAt;  // uptime at the last save, about how long the session lasted (-10 s)
  // USB
  uint16_t mounts, unmounts, suspends, relinks;
  uint32_t mountAt;
  uint32_t epAtMount, epMax, epGrowAt;  // bit n = EPn IN, bit 16+n = EPn OUT
  uint8_t serialOpened;
  // Ethernet frames from the host
  uint32_t rxFrames, rxArp, rxIpv4, rxIpv6, rxOther, rxIcmpEcho, rxMdns, rxTcp80Syn;
  uint32_t firstRxAt, firstSynAt;
  // DHCP, counted by message type 1..8 (index 0 = unknown type)
  uint16_t dhcpRx[9], dhcpTx[9];
  uint32_t dhcpDiscoverAt, dhcpAckAt;
  uint8_t clientMac[6], leaseOctet;
  char clientName[32], vendorClass[48];
  // HTTP
  uint16_t httpRequests, wsConnects;
  uint32_t firstHttpAt;
  char firstRequest[40], userAgent[160];
};

Record cur;
int curSlot = -1;
volatile bool milestone = false, dirty = false;
uint32_t lastSave = 0;

void slotPath(int slot, char *out, size_t n) { snprintf(out, n, "/flight%d.bin", slot); }

bool load(int slot, Record &r) {
  char p[20];
  slotPath(slot, p, sizeof(p));
  File f = LittleFS.open(p, "r");
  if (!f) return false;
  bool ok = f.read(reinterpret_cast<uint8_t *>(&r), sizeof(r)) == sizeof(r) && r.magic == kMagic;
  f.close();
  return ok;
}

void save(uint32_t now) {
  if (curSlot < 0) return;
  cur.savedAt = now;
  char p[20];
  slotPath(curSlot, p, sizeof(p));
  File f = LittleFS.open(p, "w");
  if (f) {
    f.write(reinterpret_cast<const uint8_t *>(&cur), sizeof(cur));
    f.close();
  }
  lastSave = now;
  milestone = dirty = false;
}

void mark(uint32_t &at) {
  if (at) return;
  at = millis();
  if (!at) at = 1;
  milestone = true;
}

void copyStr(char *dst, size_t n, const char *src) {
  strncpy(dst, src ? src : "", n - 1);
  dst[n - 1] = '\0';
}

uint32_t endpointMask() {
  uint32_t m = 0;
  for (int i = 0; i < 15; i++) {  // ep_ctrl[i] belongs to EP(i+1)
    if (usb_dpram->ep_ctrl[i].in & EP_CTRL_ENABLE_BITS) m |= 1u << (i + 1);
    if (usb_dpram->ep_ctrl[i].out & EP_CTRL_ENABLE_BITS) m |= 1u << (i + 17);
  }
  return m;
}

// ---- JSON output
void jsonStr(String &o, const char *s) {
  o += '"';
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  o += '"';
}

void jsonEp(String &o, uint32_t m) {
  o += "{\"in\":[";
  bool first = true;
  for (int n = 1; n < 16; n++)
    if (m & (1u << n)) { if (!first) o += ','; o += n; first = false; }
  o += "],\"out\":[";
  first = true;
  for (int n = 1; n < 16; n++)
    if (m & (1u << (16 + n))) { if (!first) o += ','; o += n; first = false; }
  o += "]}";
}

void kv(String &o, const char *k, uint32_t v, bool comma = true) {
  o += '"'; o += k; o += "\":"; o += v;
  if (comma) o += ',';
}

void recordJson(String &o, const Record &r, bool current) {
  static const char *const kTypes[9] = {"other", "discover", "offer", "request", "decline",
                                        "ack", "nak", "release", "inform"};
  o += "{";
  kv(o, "seq", r.seq);
  o += "\"current\":"; o += current ? "true" : "false";
  o += ",\"version\":"; jsonStr(o, r.version);
  o += ',';
  kv(o, "uptimeMs", r.savedAt);

  o += "\"usb\":{";
  kv(o, "mounts", r.mounts); kv(o, "unmounts", r.unmounts); kv(o, "suspends", r.suspends);
  kv(o, "relinks", r.relinks); kv(o, "mountAt", r.mountAt);
  o += "\"serialOpened\":"; o += r.serialOpened ? "true" : "false";
  o += ",\"epAtMount\":"; jsonEp(o, r.epAtMount);
  o += ",\"epMax\":"; jsonEp(o, r.epMax);
  o += ','; kv(o, "epGrowAt", r.epGrowAt, false);
  o += "},";

  o += "\"rx\":{";
  kv(o, "frames", r.rxFrames); kv(o, "firstAt", r.firstRxAt); kv(o, "arp", r.rxArp);
  kv(o, "ipv4", r.rxIpv4); kv(o, "ipv6", r.rxIpv6); kv(o, "other", r.rxOther);
  kv(o, "icmpEcho", r.rxIcmpEcho); kv(o, "mdns", r.rxMdns); kv(o, "tcp80Syn", r.rxTcp80Syn);
  kv(o, "firstSynAt", r.firstSynAt, false);
  o += "},";

  o += "\"dhcp\":{\"rx\":{";
  bool first = true;
  for (int t = 0; t < 9; t++)
    if (r.dhcpRx[t]) { if (!first) o += ','; kv(o, kTypes[t], r.dhcpRx[t], false); first = false; }
  o += "},\"tx\":{";
  first = true;
  for (int t = 0; t < 9; t++)
    if (r.dhcpTx[t]) { if (!first) o += ','; kv(o, kTypes[t], r.dhcpTx[t], false); first = false; }
  o += "},";
  kv(o, "discoverAt", r.dhcpDiscoverAt); kv(o, "ackAt", r.dhcpAckAt);
  char mac[18];
  snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", r.clientMac[0], r.clientMac[1],
           r.clientMac[2], r.clientMac[3], r.clientMac[4], r.clientMac[5]);
  o += "\"clientMac\":"; jsonStr(o, mac);
  o += ",\"hostName\":"; jsonStr(o, r.clientName);
  o += ",\"vendorClass\":"; jsonStr(o, r.vendorClass);
  o += ','; kv(o, "leaseOctet", r.leaseOctet, false);
  o += "},";

  o += "\"http\":{";
  kv(o, "requests", r.httpRequests); kv(o, "ws", r.wsConnects); kv(o, "firstAt", r.firstHttpAt);
  o += "\"first\":"; jsonStr(o, r.firstRequest);
  o += ",\"userAgent\":"; jsonStr(o, r.userAgent);
  o += "}}";
}

}  // namespace

void flightBegin(const char *version) {
  uint32_t maxSeq = 0;
  int maxSlot = -1;
  static Record r;  // static: keep 400 bytes off the stack
  for (int s = 0; s < kSlots; s++)
    if (load(s, r) && (maxSlot < 0 || r.seq > maxSeq)) { maxSeq = r.seq; maxSlot = s; }
  memset(&cur, 0, sizeof(cur));
  cur.magic = kMagic;
  cur.seq = maxSlot < 0 ? 1 : maxSeq + 1;
  copyStr(cur.version, sizeof(cur.version), version);
  curSlot = maxSlot < 0 ? 0 : (maxSlot + 1) % kSlots;
  save(millis());
}

void flightService(uint32_t now, bool otaActive) {
  static bool wasMounted = false, wasSuspended = false;
  bool mounted = tud_mounted();
  if (mounted && !wasMounted) {
    cur.mounts++;
    if (!cur.mountAt) cur.epAtMount = endpointMask();
    mark(cur.mountAt);
    milestone = true;
  } else if (!mounted && wasMounted) {
    cur.unmounts++;
    milestone = true;
  }
  wasMounted = mounted;

  bool suspended = tud_suspended();
  if (suspended && !wasSuspended) { cur.suspends++; milestone = true; }
  wasSuspended = suspended;

  uint32_t ep = endpointMask();
  if (ep & ~cur.epMax) {
    cur.epMax |= ep;
    cur.epGrowAt = now ? now : 1;
    milestone = true;
  }
  if (!cur.serialOpened && Serial) { cur.serialOpened = 1; milestone = true; }

  if (otaActive || curSlot < 0) return;
  uint32_t since = now - lastSave;
  if (milestone && since >= kMilestoneDelayMs) save(now);
  else if (now < kFastWindowMs && since >= kFastPeriodMs) save(now);
  else if (dirty && since >= kSlowPeriodMs) save(now);
}

void flightFrame(struct pbuf *p) {
  uint8_t h[64];
  uint16_t n = pbuf_copy_partial(p, h, sizeof(h), 0);
  cur.rxFrames++;
  mark(cur.firstRxAt);
  dirty = true;
  if (n < 14) { cur.rxOther++; return; }
  uint16_t type = (uint16_t)(h[12] << 8 | h[13]);
  if (type == 0x0806) { cur.rxArp++; return; }
  if (type == 0x86DD) { cur.rxIpv6++; return; }
  if (type != 0x0800 || n < 34) { cur.rxOther++; return; }
  cur.rxIpv4++;
  uint16_t l4 = 14 + (h[14] & 0x0F) * 4;
  uint8_t proto = h[23];
  if (proto == 1 && n > l4 && h[l4] == 8) {
    cur.rxIcmpEcho++;
  } else if ((proto == 6 || proto == 17) && n >= l4 + 4) {
    uint16_t dport = (uint16_t)(h[l4 + 2] << 8 | h[l4 + 3]);
    if (proto == 17 && dport == 5353) cur.rxMdns++;
    if (proto == 6 && dport == 80 && n >= l4 + 14 && (h[l4 + 13] & 0x12) == 0x02) {
      cur.rxTcp80Syn++;
      mark(cur.firstSynAt);
    }
  }
}

void flightDhcp(uint8_t msgType, bool rx, const uint8_t *mac, const char *hostName,
                const char *vendorClass, uint8_t leasedOctet) {
  uint8_t t = msgType <= 8 ? msgType : 0;
  if (rx) {
    cur.dhcpRx[t]++;
    if (t == 1) mark(cur.dhcpDiscoverAt);
    if (hostName && *hostName) copyStr(cur.clientName, sizeof(cur.clientName), hostName);
    if (vendorClass && *vendorClass) copyStr(cur.vendorClass, sizeof(cur.vendorClass), vendorClass);
  } else {
    cur.dhcpTx[t]++;
    if (t == 5) { mark(cur.dhcpAckAt); cur.leaseOctet = leasedOctet; }
  }
  if (mac) memcpy(cur.clientMac, mac, 6);
  dirty = true;
}

void flightHttp(const String &method, const String &path, const String &userAgent) {
  cur.httpRequests++;
  if (!cur.firstHttpAt) {
    copyStr(cur.firstRequest, sizeof(cur.firstRequest), (method + " " + path).c_str());
    mark(cur.firstHttpAt);
  }
  if (!cur.userAgent[0] && userAgent.length())
    copyStr(cur.userAgent, sizeof(cur.userAgent), userAgent.c_str());
  dirty = true;
}

void flightWs() {
  if (!cur.wsConnects) milestone = true;
  cur.wsConnects++;
  dirty = true;
}

void flightRelink() {
  cur.relinks++;
  milestone = true;
}

String flightJson() {
  static Record r;
  int order[kSlots];
  uint32_t seqs[kSlots];
  int count = 0;
  for (int s = 0; s < kSlots; s++) {
    if (s == curSlot || !load(s, r)) continue;
    int i = count++;
    while (i > 0 && seqs[i - 1] < r.seq) { seqs[i] = seqs[i - 1]; order[i] = order[i - 1]; i--; }
    seqs[i] = r.seq;
    order[i] = s;
  }
  String o;
  o.reserve(900 * (count + 1));
  o += "{\"type\":\"flight\",\"sessions\":[";
  cur.savedAt = millis();
  recordJson(o, cur, true);  // the live record is fresher than its saved copy
  for (int i = 0; i < count; i++) {
    load(order[i], r);
    o += ',';
    recordJson(o, r, false);
  }
  o += "]}";
  return o;
}

void flightClear() {
  char p[20];
  for (int s = 0; s < kSlots; s++) {
    slotPath(s, p, sizeof(p));
    LittleFS.remove(p);
  }
  save(millis());
}
