// Types used by firmware.ino. They live in a header so that the Arduino builder's
// auto-generated function prototypes (inserted at the top of the sketch) can see them.
#pragma once

#include <Arduino.h>
#include <WiFiClient.h>

struct WsConn {
  WiFiClient c;
  bool active = false;
  uint32_t since = 0;
  size_t rxLen = 0;
  uint8_t rx[512];
};

struct OtaSession {
  bool active = false;
  uint32_t total = 0, received = 0, crc = 0, startMs = 0, lastReport = 0;
  uint8_t head[256];  // first 256 bytes (boot2) are checked before anything is written
  bool begun = false;
  const char *source = "";
};

struct CmdResult {
  int code = 200;
  String body;               // reply for the requesting channel (HTTP body / Serial line / WS frame)
  bool broadcasted = false;  // body was already broadcast to every WS client and Serial
};

struct HttpReq {
  String method, path, query, wsKey, crc, userAgent;
  bool upgrade = false, expectContinue = false;
  long contentLength = -1;
};

struct WebAsset {  // static file embedded by compile.sh (firmware/webassets.h)
  const char *path, *type;
  const uint8_t *data;
  size_t len;
};
