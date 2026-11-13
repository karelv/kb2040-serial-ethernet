// Flight recorder: what happened on the USB and network side during each boot, saved in LittleFS.
// A session on a host without a serial console (iPhone, Android) can then be read back later,
// over serial or HTTP, on a computer: FLIGHT / {"cmd":"flight"} / GET /api/flight.
// The last kFlightSessions boots are kept; FLIGHT:CLEAR / {"cmd":"flightclear"} empties the log.
#pragma once

#include <Arduino.h>
#include <lwip/pbuf.h>

void flightBegin(const char *version);  // after LittleFS.begin(): starts a new session record
void flightService(uint32_t now, bool otaActive);  // from loop(): USB state polling + saving
void flightFrame(struct pbuf *p);       // every Ethernet frame from the host (lwIP context)
void flightDhcp(uint8_t msgType, bool rx, const uint8_t *mac, const char *hostName,
                const char *vendorClass, uint8_t leasedOctet);  // DhcpObserver
void flightHttp(const String &method, const String &path, const String &userAgent);
void flightWs();
void flightRelink();
String flightJson();  // {"type":"flight","sessions":[newest first]}
void flightClear();
