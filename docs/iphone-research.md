Me: Figure out what the best way is to have a website talk to Melexis.IO on a iPhone. You have under control the firmware of the STM MCU, and the https website.
Propose a concept

The cleanest zero-install architecture for connecting an **`https://` web app** to **Melexis.IO** on an iPhone is **USB `CDC-ECM` Virtual Ethernet + Split-Route `lwIP` + Browser-Provisioned Per-Device `WSS` (`mbedTLS`)**, exposing the board's existing SCPI and sensor stream over a Secure WebSocket (`wss://local.usb.melexis.io/ws`).

Because every browser on iOS runs on WebKit, **WebSerial, WebUSB, WebHID, WebBluetooth, and WebMIDI are all unavailable** on iPhone. At the same time, serving your app from an `https://` origin means WebKit's Mixed Content policy **strictly blocks** plain `http://` `fetch()` and `ws://` WebSocket connections—even to `localhost`, `.local` mDNS, or private `192.168.x.x` USB addresses—with no user bypass. Pairing OS-native USB Ethernet with per-device automated TLS solves both walls simultaneously without an App Store app or MFi hardware.

---

## Architecture Comparison on iPhone

| Approach | Zero App / Zero MFi | Works from `https://` Origin | iPhone / STM32F446 Reality Check |
| --- | --- | --- | --- |
| **USB `CDC-ECM` + Per-Device `WSS` (Recommended)** | Yes | Yes | Mounts natively in iOS in under 1 s; fits in ~98 KB Flash / ~42 KB SRAM on STM32F446ME; keeps 5G/Wi-Fi internet live. |
| **WebRTC `RTCDataChannel` over USB Ethernet** | Yes | Yes (DTLS fingerprint) | **Fails cleanly on WebKit:** iOS suppresses private-IP ICE pairing unless the site requests Camera/Mic (`getUserMedia`) permission, and `usrsctp` bloats SRAM. |
| **Serve Entire UI over Plain `HTTP` from MCU** | Yes | No (loses `https://` origin) | Loses cloud updates and Secure Context APIs, squeezes the 512 KB STM32F446 Flash, and triggers Safari HTTP security warnings if enabled. |
| **Shared Wildcard TLS Private Key in Firmware** | Yes | Temporarily | **Fatal security flaw:** Extractable shared keys in distributed firmware violate CA/Browser Forum rules (mandatory 24-hour global revocation) and expire on the shelf. |
| **BLE Daughterboard + WebBLE Browser App** | No | Yes (in wrapper app) | Requires extra radio BOM (STM32F446 has no BLE) plus installing a third-party App Store browser, with low throughput for thermal frames. |

---

## Proposed System Concept: Four Technical Pillars

### 1. Physical & USB Layer: Composite `CDC-ECM` + `CDC-ACM` on STM32F446

The Melexis.IO board runs an **STM32F446ME** (180 MHz Cortex-M4F, 512 KB Flash, 128 KB SRAM) with USB Type-C wired to `USB_OTG_FS` (`PA11`/`PA12`, 12 Mbps Full-Speed, required by the STM32 ROM DFU bootloader).

* **Prefer `CDC-ECM` over `CDC-NCM` for iOS:** While `CDC-NCM` aggregates frames, TinyUSB + `CDC-NCM` exhibits intermittent DHCP lease failures on iOS 26, whereas Apple's built-in `CDC-ECM` driver mounts reliably across iOS 18–26 and macOS.
* **Preserve Desktop `WebSerial` Compatibility:** Using TinyUSB Interface Association Descriptors (IAD), the STM32F446 can expose **both** `CDC-ECM` (for iPhone, iPad, and macOS Safari) and `CDC-ACM` (for existing Desktop Chrome/Edge `WebSerial` and Python SCPI scripts) simultaneously.
* **Exact `USB_OTG_FS` Endpoint & FIFO Fit:** `USB_OTG_FS` provides `EP0` plus **5 bidirectional non-control endpoints** and **1,280 bytes (320 32-bit words)** of dedicated USB FIFO RAM. Composite `CDC-ECM + CDC-ACM` consumes **4 IN and 2 OUT endpoints** and fits the hardware FIFO to the exact word:
* `Shared RxFIFO`: **64 words** (256 B)
* `EP0 TxFIFO` (Control): **32 words** (128 B)
* `EP1 TxFIFO` (ECM Notification IN): **16 words** (64 B hardware minimum)
* `EP2 TxFIFO` (ECM Bulk Data IN): **128 words** (512 B — multi-buffered for full 12 Mbps streaming)
* `EP3 TxFIFO` (ACM Notification IN): **16 words** (64 B)
* `EP4 TxFIFO` (ACM Bulk Data IN): **64 words** (256 B)


* **iOS USB Power Budget:** Set `bMaxPower` to **100 mA** in the USB Configuration Descriptor so iOS enumerates immediately without tripping accessory overcurrent rejection (even on older iPhones via a Lightning-to-USB adapter).

### 2. Network Layer: Split-Route DHCP (Keeping iPhone 5G & Wi-Fi Alive)

If a USB Ethernet adapter advertises a Default Gateway, iOS prioritizes wired Ethernet over Wi-Fi and Cellular—instantly blackholing the iPhone's internet connection.

* **Point-to-Point `/30` Subnet:** Run `lwIP` (`NO_SYS=1`) with a minimal DHCP server assigning **`192.168.73.1`** to Melexis.IO and **`192.168.73.2`** (`255.255.255.252`) to the iPhone.
* **Omit DHCP Option 3 & Option 6:** Do **not** send DHCP Option 3 (Router/Default Gateway) or Option 6 (DNS) during normal operation. iOS installs a link-scoped route exclusively for `192.168.73.0/30` over USB while routing all internet traffic and DNS queries over **5G or Wi-Fi simultaneously**.
* **Wildcard Public DNS + Offline PWA Fallback:** Publish a single static wildcard record on your domain (`*.usb.melexis.io IN A 192.168.73.1`). Cache the web app via a **Service Worker (PWA)**, and run a lightweight UDP port 53 responder on `192.168.73.1` as a secondary fallback so already-paired boards continue resolving `local.usb.melexis.io` when the iPhone has zero cellular or Wi-Fi signal in the field.

### 3. Security Layer: Browser-Relayed ACME TLS (Zero Shared Keys, 200-Day Ready)

Two industry rules break traditional embedded HTTPS designs:

1. **No Shared Private Keys:** Shipping one shared TLS private key across firmware binaries leads to mandatory CA revocation within 24 hours if extracted.
2. **Shrinking Certificate Lifetimes:** Effective March 15, 2026, maximum public TLS certificate validity dropped to **200 days** (shrinking to **100 days in March 2027** and **47 days in 2029**). Any factory-flashed certificate will expire in a customer's drawer.

Because you control **both** the STM32 firmware and the `https://` backend, the browser itself can act as an automated zero-touch certificate relay:

1. **On-Chip Non-Exportable Key Generation (Solving the STM32F446 RNG Gap):**
* Unlike the STM32F429 or STM32L4, the **STM32F446 has no hardware TRNG peripheral**.
* On first bootstrap, the `https://` website generates 32 bytes of cryptographic entropy via `window.crypto.getRandomValues()` and passes it to the MCU, where firmware mixes it with the STM32's 96-bit Unique ID (`UID_BASE`), uninitialized startup SRAM bits, and internal ADC temperature/`Vrefint` jitter inside `mbedtls_ctr_drbg`.
* The STM32 generates a unique **ECDSA P-256 private key** on-chip (stored in a dedicated **16 KB lower Flash sector**—e.g., Sector 2 or 3—avoiding 128 KB upper-sector erase stalls). **The private key never leaves the STM32.**


2. **Dual-SAN ACME Issuance (`local.usb.melexis.io` + `<uid>.usb.melexis.io`):**
* Your backend (`[https://api.melexis.io/acme/sign](https://api.melexis.io/acme/sign)`) validates `DNS-01` automatically and signs the STM32's 180-byte Certificate Signing Request (CSR) via Let's Encrypt or Google Trust Services with two Subject Alternative Names: **`local.usb.melexis.io`** (universal zero-config entrypoint) and **`<uid>.usb.melexis.io`** (unique per-board identity that also avoids duplicate-SAN CA rate limits).


3. **Fast Path vs. Auto-Renewal Bounce:**
* **Steady State (99% of sessions, ~50 ms):** On any iPhone, opening `[https://www.melexis.io](https://www.melexis.io)` immediately connects to **`wss://local.usb.melexis.io/ws`**. Because every board holds a valid CA-signed cert covering `local.usb.melexis.io`, Safari connects silently with **zero redirects, zero popups, and zero iOS Local Network permission prompts** (Safari traffic is exempt per Apple TN3179).
* **First Boot or Expired Cert (~3 s):** If `wss://` fails because the 200-day cert expired, tapping **Connect** triggers a top-level navigation to `[http://192.168.73.1/bootstrap?seed=](http://192.168.73.1/bootstrap?seed=)...` (permitted by WebKit because top-level navigation creates a new browsing context rather than Mixed Content). The STM32 immediately `302`-redirects back to `[https://www.melexis.io/#csr=](https://www.melexis.io/#csr=)...`, the web app fetches the fresh ~1.4 KB ECDSA cert chain from your ACME endpoint, installs it via one `302` return bounce, and opens `wss://`. *(Bonus: Desktop `WebSerial` sessions can check `:SYSTem:TLS:EXPiry?` silently in the background and push fresh certs over serial whenever validity drops under 60 days.)*



### 4. Application Layer & STM32F446 Memory Budget

Abstract the web app's hardware interface behind a dual-mode `MelexisTransport` class (`WebSerialTransport` on Desktop Chromium; `WebSocketTransport` on iOS/iPadOS/macOS Safari and Firefox):

* **Hybrid WebSocket Framing:** Send standard `\n`-terminated **SCPI commands** (`*IDN?`, `:EEPROM:STRing`) as WebSocket **Text frames**, and stream high-rate sensor payloads (such as 32×24 MLX90642 thermal frames or high-speed encoder captures) as raw **Binary frames (`ArrayBuffer`)** to eliminate ASCII/hex serialization overhead.
* **Tight `mbedTLS` Profile:** Restrict `mbedTLS` strictly to **TLS 1.3 / 1.2 Server** with `TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256` and 4 KB I/O fragment buffers instead of default 16 KB buffers.

| Firmware Module | Flash Footprint (512 KB Total) | SRAM Footprint (128 KB Total) |
| --- | --- | --- |
| **TinyUSB (`CDC-ECM` + `CDC-ACM`)** | ~14 KB | ~6 KB |
| **`lwIP` (TCP/UDP + Split-DHCP + Mini-DNS)** | ~35 KB | ~20 KB |
| **`mbedTLS` (ECDSA P-256 + AES-128-GCM, 4 KB buffers)** | ~45 KB | ~14 KB peak (~8 KB active) |
| **HTTP Bootstrap + WebSocket SCPI Bridge** | ~6 KB | ~4 KB |
| **Total Communication Stack** | **~100 KB (leaves ~396 KB + 16 KB key sector)** | **~44 KB peak (leaves ~84 KB for sensor DSP)** |

---

Explore the end-to-end communication stack below—use the **Prev / Next** stage controls to step through USB enumeration, first-time/expired CSR bootstrap, and steady-state `WSS` streaming, or click any subsystem block to inspect its exact hardware and protocol configuration:

---

> **Key architectural takeaway:** Including `local.usb.melexis.io` alongside `<uid>.usb.melexis.io` in each board's unique, on-chip-keyed certificate gives you the best of both worlds—**zero shared private keys** (so a single compromised board can never revoke the fleet) and **zero-redirect instant connection** on any iPhone without needing to know the board's serial number beforehand.

Me: How confident are you this will work? 
Is there any benefit to include Arduino platform?

I am **85–90% confident** in the raw architecture as proposed, rising to **over 95% for production** if—and only if—you engineer upfront for **four real-world edge cases** (most notably corporate Wi-Fi DNS rebinding filters and public CA rate limits).

As for **Arduino**: **do not use it (`STM32duino`) to build the production STM32F446 USB/TLS firmware**—unlike on RP2040 or ESP32-S3, Arduino on STM32F4 will actively fight you on `TinyUSB`, `lwIP`, and SRAM determinism. Its only real value here is either **de-risking the iOS browser flow in 48 hours** on an already-supported MCU, or offering an **optional customer-flashable sandbox mode** via WebDFU.

---

## Part 1: Engineering Confidence & The 4 Real-World Traps

Every individual building block is proven in production—Apple's built-in `AppleUSBECM` driver requires no MFi chip, omitting DHCP Option 3 preserves iPhone 5G/Wi-Fi routing, and resolving public DNS names to `192.168.x.x` for local TLS is the exact pattern Plex (`*.plex.direct`) uses across millions of iOS devices.

Where the remaining 10–15% risk hides is in the interaction between **network middleboxes, CA policies, and desktop coexistence**:

| Subsystem | Confidence | Primary Failure Mode | Concrete Engineering Mitigation |
| --- | --- | --- | --- |
| **iOS USB `CDC-ECM` + Split `/30` DHCP** | **98%** | Drawing over 100 mA before enumeration or accidentally sending DHCP Option 3 (Gateway). | Set `bMaxPower = 100 mA`; serve only IP, `/30` mask, and lease time in DHCP ACK. |
| **STM32F446 Flash/SRAM Fit & Single-Bank Erase** | **95%** | Erasing a 16 KB sector on the single-bank F446 stalls Flash instruction fetches for ~200–400 ms, dropping live USB interrupts. | Write keys/certs **only** at the end of the HTTP bootstrap step, then trigger a clean `NVIC_SystemReset()` (or place the USB ISR in `.RamFunc`). |
| **HTTP `302` CSR Bootstrap & On-Chip ECDSA** | **92%** | URL fragment bloat or blocking the USB loop during ~20 ms Cortex-M4F ECDSA P-256 signing. | Base64url-encoded P-256 CSRs are ~260 bytes (well within Safari URL limits); run `mbedTLS` strictly in the main loop, never in `OTG_FS_IRQHandler`. |
| **Public DNS to `192.168.73.1` (`*.usb.melexis.io`)** | **75% raw → 96% mitigated** | **Corporate Wi-Fi routers** (`dnsmasq`, Unbound, pfSense, Umbrella) with **DNS Rebinding Protection** strip `192.168.x.x` `A` records; **iCloud Private Relay** can also interfere with private-IP resolution. | 1. Detect `wss://` DNS failure while `[http://192.168.73.1](http://192.168.73.1)` is reachable and prompt the user to toggle Wi-Fi off (5G/LTE bypasses corporate router DNS filters).<br>

<br>2. Serve a tiny plain-HTTP emergency UI at `[http://192.168.73.1](http://192.168.73.1)` so the board works even in an offline Faraday lab. |
| **Public ACME Certificate Issuance at Scale** | **70% on LE → 98% on GTS** | Let's Encrypt caps new certificates at **50 per registered domain per 7 days** (refilling 1 every 202 min). Provisioning 60 boards in a week hits a hard wall. | Use **Google Trust Services ACME** (**100 new orders/hour** per GCP project, no 50/week domain cap), or request a Let's Encrypt rate-limit override on a dedicated domain (`melexis-usb.io`) before launch. |
| **Desktop Coexistence (Windows + Chrome)** | **85% → 98%** | Windows 10/11 lacks a built-in `CDC-ECM` driver (showing a Device Manager warning icon alongside working `CDC-ACM`), and Chrome 142+ / Firefox 151+ gate `https://` → `192.168.x.x` behind a Local Network Access prompt. | Expose **Dual USB Configurations** (`bNumConfigurations = 2`: Config 1 = `CDC-ACM` / `RNDIS` for Windows; Config 2 = `CDC-ECM + CDC-ACM`, which macOS and iOS automatically select), and keep Desktop browsers on `WebSerial`. |

---

## Part 2: Is There Any Benefit to Including the Arduino Platform?

This depends on whether you mean **building the STM32F446 production firmware on Arduino** or **using Arduino for rapid prototyping / customer extensibility**.

### 1. For the Production STM32F446 Firmware: No (Net Negative)

If you have worked with Arduino on **RP2040 (`earlephilhower/arduino-pico`)** or **ESP32-S3**, TinyUSB + `lwIP` USB Ethernet feels effortless because those cores integrate TinyUSB natively. **On STM32, that ecosystem does not exist:**

* **Experimental TinyUSB Support:** `stm32duino` (`Arduino_Core_STM32`) defaults to ST's legacy USB Device middleware, which lacks clean composite `CDC-ECM + CDC-ACM`. `Adafruit_TinyUSB_Arduino` explicitly classifies `Arduino_Core_STM32` as **experimental / without built-in support**—preliminary STM32F4 `OTG_FS` support only landed in v3.7.4 (May 2026) and requires manual core patching and custom `TinyUSB_Device_Task()` plumbing.
* **Wrong Ethernet Plumbing:** `STM32duino`'s `STM32Ethernet` / `LwIP` library is hardcoded for STM32 chips with a hardware Ethernet MAC (`ETH` peripheral on F407/F429/H7), which the **STM32F446ME lacks**. You would still have to wire `TinyUSB` netif callbacks into raw `lwIP` and `mbedTLS` yourself in C.
* **SRAM & Heap Fragility:** With only 128 KB of SRAM shared across USB DMA/FIFOs, `lwIP` `pbuf` pools, `mbedTLS` handshake state, and high-rate sensor buffers, Arduino's hidden C++ allocations (`String`, `HardwareSerial` ring buffers, non-deterministic heap fragmentation) add risk without saving engineering effort.

| Firmware Stack Option | Fit for Melexis.IO STM32F446 | Why |
| --- | --- | --- |
| **STM32Cube C / CMake (LL/HAL + TinyUSB + `lwIP` + `mbedTLS`)** | **Best Choice** | TinyUSB's native Synopsys `dwc2` port (`portable/synopsys/dwc2`) is mature on STM32F4 `OTG_FS`; static allocation gives 100% deterministic control over all 128 KB SRAM and 320 FIFO words. |
| **Zephyr RTOS** | **Strong Runner-Up** | Ships integrated USB `CDC-ECM` + `CDC-ACM`, DHCPv4 server, `mbedTLS`, and HTTP/WebSocket server out of the box, though base Flash/SRAM overhead is heavier than bare-metal `NO_SYS=1`. |
| **Arduino (`Arduino_Core_STM32`)** | **Avoid for Core Stack** | Experimental TinyUSB port, no `CDC-ECM`-to-`lwIP` bridge, and C++ heap overhead. |

### 2. Two Places Where Arduino *Does* Add Real Value

1. **A 48-Hour iOS Safari Proof-of-Concept (Before Writing STM32 C Code):**
Before investing weeks into STM32F446 C firmware, you can flash an **RP2040** (`arduino-pico` has native TinyUSB USB-Ethernet + `lwIP` + TLS) or configure a **Linux / Raspberry Pi Zero `g_ether` USB gadget** in an afternoon. Plugging that into an iPhone lets you empirically validate **iOS 26 Safari + iCloud Private Relay + corporate Wi-Fi + ACME `wss://` handshake** in two days and bring your architectural confidence to 99% before touching STM32 registers.
2. **Optional Customer "Sandbox Mode" via Browser WebDFU:**
Because the STM32F446ME's `PA11`/`PA12` USB pins connect directly to ST's **built-in ROM DFU bootloader**, you can publish a lightweight `Arduino_Core_STM32` board definition for Melexis.IO alongside Melexis Arduino sensor libraries. Engineers evaluating a sensor can write custom standalone Arduino sketches (using simple `Serial` `CDC-ACM`) and flash `.bin` builds straight from your website over WebUSB/WebDFU on desktop—and click "Restore Factory Melexis.IO Firmware" whenever they want the full SCPI + iPhone `WSS` bridge back.

Me: I like the idea of a POC with rp2040.
Can this poc be done at a POC repo with https served by GitLab pages?
My special concern is about:
Public DNS to 192.168.73.1 (*.usb.melexis.io)

Me: Would it be easier if we say that only offline pwa on iPhone has to work, while keeping webserial for the other platforms?

**Yes—you can run the entire RP2040 PoC from a static GitLab Pages repository (`https://<namespace>.gitlab.io/<poc-repo>`) without touching `melexis.io` DNS, spinning up a backend server, or involving corporate IT.**

Your instinct to question **`*.usb.melexis.io` → `192.168.73.1**` is also right on target. Pointing a corporate subdomain at a private RFC 1918 IP bundles together **domain-isolation risks** (easy to fix) and **corporate Wi-Fi DNS-rebinding blocks** (the #1 real-world failure mode you should test in this PoC).

---

## 1. Why GitLab Pages Works Cleanly for the PoC

* **Cross-Origin `WSS` Is Natively Allowed:** Unlike `fetch()`, browser `WebSocket` connections are not blocked by cross-origin CORS preflights. A static page hosted on `https://<namespace>.gitlab.io/<poc-repo>` can open `wss://<any-other-domain>/ws` directly—WebKit simply sends `Origin: https://<namespace>.gitlab.io` during the HTTP Upgrade handshake for your RP2040 firmware to allowlist. Standard GitLab Pages hosting does not inject restrictive `connect-src` Content-Security-Policy headers unless you add them yourself.
* **No Live ACME Server Needed for the PoC:** Because you only need a certificate for 1–2 RP2040 test boards right now, you can skip building the dynamic CSR/ACME cloud backend initially. Run `acme.sh` or `certbot` **once on your laptop** using a `DNS-01` challenge, drop the resulting 200-day ECDSA P-256 certificate and private key into RP2040 Flash (`LittleFS`), and host pure static HTML/JS on GitLab Pages.

---

## 2. Unpacking Your Concern: Public DNS → `192.168.73.1`

There are three distinct issues hidden behind `*.usb.melexis.io` → `192.168.73.1`:

### A. Corporate Governance & Cookie Isolation (Never Use `melexis.io`)

Even in production—and especially in a PoC—**never place `192.168.73.1` records under `melexis.io` or `melexis.com**`:

* Any subdomain of `melexis.io` shares the same eTLD+1 ("same-site") boundary, creating a risk of `Domain=.melexis.io` cookies leaking toward a local network IP.
* **Fix:** Use a **completely separate, cookie-less domain**. For a zero-cost GitLab Pages PoC, create a free **DuckDNS** hostname (e.g., `mlx-poc.duckdns.org` → `192.168.73.1`, which supports Let's Encrypt `DNS-01` via API token), or register a $2 Cloudflare throwaway domain (`*.mlx-usb.net`). *(Avoid shared IP-encoding domains like `sslip.io` or `nip.io`—they do not support wildcard `DNS-01` certs and routinely hit shared Let's Encrypt rate limits.)*
* **Is static `A 192.168.73.1` a DNS Rebinding vulnerability?** No. Classic DNS rebinding attacks work by flipping an attacker domain from a public IP (short TTL) to `192.168.x.x` to read unauthenticated plain-HTTP routers under the attacker's origin. Here, the isolated domain statically resolves to `192.168.73.1`, serves no active scripts, requires a **TLS handshake** backed by the MCU's private key, and verifies the `Origin` header from `gitlab.io`.

### B. The Real Catch: Corporate Wi-Fi "DNS Rebinding Protection"

This is the genuine fragility of mapping *any* public domain to `192.168.73.1`:

* Enterprise DNS resolvers (Cisco Umbrella, Infoblox) and prosumer firewalls (OpenWrt `dnsmasq --stop-dns-rebind`, Pi-hole, pfSense/OPNsense Unbound `private-address`, Fritz!Box) actively inspect upstream DNS replies and **silently drop `A` records inside `192.168.0.0/16`, `10.0.0.0/8`, and `172.16.0.0/12**`.
* **Why USB DHCP can't silently override just that one domain on iOS:** When your USB Ethernet interface omits DHCP Option 3 (Default Gateway) so iPhone 5G/Wi-Fi stays alive, iOS keeps Wi-Fi/5G as the Primary interface. Apple's `mDNSResponder` sends `.local` queries over multicast DNS on all interfaces, and sends **all standard unicast domain lookups to the Primary (Wi-Fi/5G) DNS server**—ignoring unicast DNS servers on secondary router-less USB links. Meanwhile, public CAs are barred from issuing certificates for `.local` or raw `192.168.73.1`.

---

## 3. Three DNS/TLS Options to Test in Your RP2040 PoC

Because you have an RP2040 PoC before locking STM32 firmware, you can empirically test how your target iPhones and office Wi-Fi behave across these three approaches:

| Strategy | Public DNS Needed? | Zero iOS Setup? | Corporate Wi-Fi (`stop-dns-rebind`) | Best Role |
| --- | --- | --- | --- | --- |
| **1. Isolated Sandbox Domain (`mlx-poc.duckdns.org` → `192.168.73.1`)** | Yes (free DuckDNS or $2 Cloudflare) | **Yes** | **Blocked on strict corporate Wi-Fi** (works immediately on 5G/LTE, home Wi-Fi, or iOS DoH profile) | **Primary PoC & Production Fast-Path** |
| **2. PWA Offline Cache + USB Local-DNS Fallback (DHCP Option 3 + 6)** | Only on first cert issue | **Yes** | **Works (when Wi-Fi toggled off / Offline Mode)** — RP2040 answers UDP 53 locally once GitLab Pages PWA is cached | **Field / Faraday-Lab / Corporate-Blocked Fallback** |
| **3. Private Root CA (`mkcert`) + mDNS (`wss://mlx-poc.local/ws`)** | **None at all** | **No** (one-time `.mobileconfig` CA trust in iOS Settings) | **Works 100% everywhere** (`.local` uses link-local UDP 5353 over USB, bypassing Wi-Fi DNS completely) | **Internal Engineering / Day-1 Lab PoC** |

---

## 4. RP2040 (`arduino-pico`) PoC Implementation Watch-Outs

Using Earle Philhower's `arduino-pico` core, all the pieces exist in a single repo, with three details to wire carefully:

1. **You Must Run a Split-DHCP Server on the RP2040:** `arduino-pico`'s built-in `NCMEthernetlwIP` (`#include <NCMEthernet.h>`) defaults to acting as a **DHCP client** expecting a PC host to share internet. An iPhone acts as a **DHCP client** on USB Ethernet—so configure the RP2040 with static IP `192.168.73.1` and run a lightweight UDP port 67/68 DHCP responder handing out `192.168.73.2` (`255.255.255.252`) **without DHCP Option 3 (Router)**.
2. **Test Both `CDC-NCM` and `CDC-ECM` on Your Target iOS Version:** `arduino-pico` exposes `NCMEthernet` out of the box. Test plug/unplug reliability 10 times in a row on your iPhone; if iOS stalls renewing a `CDC-NCM` lease on hot-replug, switch TinyUSB to `CFG_TUD_ECM_RNDIS = 1` (`CDC-ECM`), which directly mirrors what you will deploy on the STM32F446.
3. **`BearSSL` ECDSA P-256 over Cortex-M0+:** Avoid RSA-2048 keys on RP2040 (handshake takes over 1.5 seconds without hardware crypto). Generate an **ECDSA P-256** key (`prime256v1`)—`BearSSL` completes the TLS 1.2 ECDHE-ECDSA handshake in **~200 ms** on RP2040 at 133–200 MHz.

---

Use the interactive routing simulator below to toggle between **iPhone Network Environments** (5G, Standard Wi-Fi, Corporate Wi-Fi with DNS Rebinding Protection, and Offline Lab) and **DNS/TLS Strategies** to trace exactly where DNS lookups and `WSS` frames travel from your GitLab Pages app:

---

> **Recommended PoC Game Plan:** Start with **`mlx-poc.duckdns.org` → `192.168.73.1**` (zero cost, zero `melexis.io` exposure, zero iOS profile install) and test your GitLab Pages site on your iPhone over **both 5G and your office Wi-Fi**. You will find out on day one whether your corporate Wi-Fi strips RFC 1918 DNS records—and if it does, your PoC UI can automatically detect the DNS failure (by probing an image/beacon or timing out `wss://` while USB link is present) and guide the user cleanly.

Would it be easier if we say that only offline pwa on iPhone has to work, while keeping webserial for the other platforms?

**Yes—making iPhone offline-only eliminates the public `A 192.168.73.1` DNS record, corporate Wi-Fi DNS-rebinding issues, and split-routing complexity, *with one crucial WebKit catch*: an offline `https://` PWA still requires `WSS` (TLS) on the MCU.**

Because Service Workers can only be installed from an `https://` origin, a cached PWA launched in Airplane Mode still runs inside an `https://` security context—and WebKit strictly blocks plain `ws://` and `http://` subresource connections to `192.168.x.x` as Mixed Content even when completely offline. Meanwhile, keeping `WebSerial` for your online `https://` build covers Desktop Chrome/Edge/Opera as well as **Android Chrome 148+ (which added native Web Serial support in April 2026, alongside existing WebUSB polyfills)**.

---

## 1. What "Offline `HTTPS` PWA on iPhone" Simplifies—and What Remains

### What Gets Much Easier

* **Zero `192.168.73.1` Record in Public DNS:** ACME `DNS-01` certificate validation only checks a `_acme-challenge` `TXT` record—you **never** have to publish an `A 192.168.73.1` record on the public internet. When offline, the MCU's DHCP server includes **Option 3 (Gateway `192.168.73.1`)** and **Option 6 (DNS `192.168.73.1`)**, and a tiny ~60-line UDP port 53 responder on the MCU resolves `usb.mlx-poc.org` to `192.168.73.1` directly over the USB cable.
* **Zero Corporate Wi-Fi `stop-dns-rebind` Failures:** Because DNS is answered locally over USB while offline, enterprise resolvers never see or strip private RFC 1918 replies.
* **Desktop `WebSerial` Can Silently Renew Certificates:** Since Desktop and Android users connect to the live `https://` site via `WebSerial`, the desktop app can check `:SYSTem:TLS:EXPiry?` in the background and push a fresh 200-day certificate over serial—letting you skip the iPhone HTTP `302` CSR bootstrap flow entirely if boards occasionally touch a PC.

### The 3 Remaining Catches of an Offline `HTTPS` PWA

1. **TLS (`BearSSL` / `mbedTLS`) Stays in Firmware:** You still carry ~45 KB of TLS Flash + a 16 KB key sector on STM32F446, and a board sitting untouched in a drawer for over 200 days still needs a PC `WebSerial` connection (or brief online session) to refresh its certificate.
2. **"Add to Home Screen" Before Going Offline Is Mandatory:** Safari's Intelligent Tracking Prevention deletes standard browser-tab Service Worker registrations and Cache Storage after **7 days of Safari use** without visiting the site. Users **must** visit your GitLab Pages URL while online and tap **Share → Add to Home Screen**, which places the PWA in an isolated container exempt from Safari's 7-day storage purge.
3. **Wi-Fi / Cellular Must Actually Be Off:** If the user leaves Wi-Fi on, iOS's `captivenetworkd` probes `[http://captive.apple.com/hotspot-detect.html](http://captive.apple.com/hotspot-detect.html)` over USB, detects no internet access, and falls back to Wi-Fi for DNS. Without a public `A` record, the PWA UI should prompt the user to enable **Airplane Mode** (or toggle Wi-Fi off) if `wss://` resolution fails.

---

## 2. The Even Simpler Alternative This Unlocks: MCU-Served `[http://mlx.local](http://mlx.local)`

Once you decide **iPhone only needs to work offline**, ask one question: *does the iPhone need an `https://` PWA at all, or can the MCU serve the gzipped UI directly over plain `[http://mlx.local](http://mlx.local)`?*

With a **single frontend codebase** in GitLab CI, you can output two targets automatically:

1. **GitLab Pages (`https://`):** Full PWA with `WebSerial` for Windows, macOS, Linux, and Android.
2. **Embedded C Array (`index.html.gz`):** Stripped, gzipped single-file bundle served directly by the RP2040 / STM32 over plain `[http://mlx.local](http://mlx.local)` communicating via plain `ws://mlx.local/ws` on iPhone.

* **Why this is radically simpler on iPhone:**
* **Zero `mbedTLS` / `BearSSL**` (frees **~62 KB of Flash and ~14 KB of SRAM** on STM32F446).
* **Zero certificates & infinite shelf life**—never expires after 200 days in storage.
* **Zero pre-installation**—works on any unconfigured iPhone in a Faraday cage out of the box, and works **even with Corporate Wi-Fi left ON** because `mlx.local` resolves via link-local mDNS (UDP 5353) over USB. You can even advertise `[http://mlx.local](http://mlx.local)` via **DHCP Option 114** (RFC 8910 Captive Portal API) or an NFC sticker.


* **The Only Trade-Off:** Your gzipped iPhone web UI must fit inside MCU Flash (effortless on RP2040's 2–8 MB Flash; capped around **250–300 KB gzipped** on the 512 KB STM32F446ME), and UI updates on iPhone follow firmware updates rather than cloud pushes.

---

## Architecture Comparison Under Your New Constraint

| Criteria | 1. Online Hybrid `WSS` (Original) | 2. Offline `HTTPS` Home-Screen PWA + `WSS` | 3. MCU-Hosted `[http://mlx.local](http://mlx.local)` (`WS`) + Cloud `WebSerial` |
| --- | --- | --- | --- |
| **Public `A 192.168.73.1` DNS Needed?** | Yes | **No** (MCU answers UDP 53 offline) | **No** (Bonjour mDNS `.local` over USB) |
| **TLS & 200-Day Cert Renewal?** | Required | **Still Required** (WebKit Mixed Content) | **Eliminated (0 certs, never expires)** |
| **Prior Online Setup on iPhone?** | None (needs live internet) | **Mandatory** (`Add to Home Screen` while online) | **None** (works cold out of the box) |
| **Works with Corporate Wi-Fi Left ON?** | Blocked by `stop-dns-rebind` | No (requires Airplane Mode / Wi-Fi off) | **Yes** (`mlx.local` uses USB multicast DNS) |
| **Web UI Size Limit** | Unlimited (GitLab Pages) | ~50 MB iOS PWA cache | **~280 KB gzipped** on STM32F446 (MBs on RP2040) |
| **STM32F446 Comms Stack Flash** | ~116 KB (incl. key sector) | ~118 KB (incl. key sector) | **~56 KB** (+ gzipped UI bundle) |

---

Adjust the **Gzipped Web UI Size** and **Operational Priorities** below to see where the **512 KB STM32F446ME Flash budget** crosses over between serving the app from MCU Flash (`[http://mlx.local](http://mlx.local)`) versus caching an Offline `HTTPS` PWA (`WSS`):

---

**Which way should your RP2040 + GitLab Pages PoC go?**

* Choose **MCU-Hosted `[http://mlx.local](http://mlx.local)` (`WS`) + GitLab Pages `WebSerial**` if your iPhone UI is a focused instrument panel (under ~250 KB gzipped—e.g., Preact/Lit + `uPlot` + SCPI console). You skip TLS, certificates, DNS domains, and Home-Screen setup altogether.
* Choose **Offline `HTTPS` Home-Screen PWA + Local USB DNS (`WSS`)** if your web app will grow into a multi-megabyte suite (3D visualizers, embedded PDF datasheets, large JS frameworks) that will never fit inside 512 KB of STM32F446 Flash.

Me: Melexis.io is limited in flash storage.
So serving the entire portal from http is not an option. However can http get their content from https?

Also tell me more about that little 60 line script, where does it run? I don't want that on the laptop, and I don't see how that can run on the phone, so can that be run on the MCU?

**Yes to both:** an `http://` page served by the MCU can freely load all of its JavaScript, CSS, images, and data files from an `https://` server, and that "~60-line responder" is **compiled C code running directly inside the MCU firmware**—nothing runs on a laptop or iPhone.

In fact, flipping the design so the MCU serves a **~500-byte `[http://mlx.local](http://mlx.local)` HTML shell** that pulls your full web app from **`https://` GitLab Pages** solves the STM32F446 Flash limit while completely eliminating TLS (`mbedTLS`), certificates, 200-day renewals, and public `192.168.73.1` DNS records.

---

## 1. How the "`[http://mlx.local](http://mlx.local)` Shell + `https://` Cloud Bundle" Works

Browser Mixed Content rules are strictly **one-way**: an `https://` page is forbidden from loading `http://` or `ws://` resources, whereas an `http://` page is **universally allowed** to load `https://` scripts, stylesheets, images, and `fetch()` payloads.

### The Execution Flow on iPhone

1. **Tiny Static Shell on the MCU (~500 Bytes of Flash):**
The MCU's HTTP server only stores a minimal `index.html` pointing to your deployed GitLab Pages bundle:
```html
<!doctype html>
<html>
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <link rel="stylesheet" href="https://<group>.gitlab.io/<poc>/app.css">
  </head>
  <body>
    <div id="app">Connecting to Melexis.IO...</div>
    <script src="https://<group>.gitlab.io/<poc>/app.iife.js"></script>
  </body>
</html>

```


2. **Cloud Assets Execute Inside the MCU's Origin (`[http://mlx.local](http://mlx.local)`):**
Even though Safari downloads megabytes of `app.iife.js` and `app.css` over 5G or Wi-Fi from `https://<group>.gitlab.io`, JavaScript always inherits the **origin of the HTML document that loaded it** (`[http://mlx.local](http://mlx.local)`).
3. **Same-Origin Plain `ws://mlx.local/ws` Over USB:**
Because the running app's origin is `[http://mlx.local](http://mlx.local)`, calling `new WebSocket("ws://mlx.local/ws")` is a **same-origin plain WebSocket**. No TLS library, private keys, or CA certificates are needed on the STM32F446—saving ~61 KB of Flash and ~14 KB of SRAM—and you can push UI updates to GitLab Pages anytime without reflashing the board. *(Using the Bonjour hostname `ws://mlx.local/ws` rather than a bare IP `ws://192.168.73.1/ws` also sidesteps a Safari/iOS 26 quirk where IP-literal WebSockets can fail to connect.)*

### Four Rules to Make This Architecture Work Cleanly

* **Rule 1 — Live 5G/Wi-Fi Is Required on iPhone (Omit DHCP Option 3):** Because `[http://mlx.local](http://mlx.local)` is not an `https://` origin (`window.isSecureContext === false`), Safari disables **Service Workers** on `[http://mlx.local](http://mlx.local)`. That means you cannot permanently pre-cache it as an offline Home-Screen PWA—the iPhone needs active 5G or Wi-Fi to load `app.iife.js` from GitLab Pages. Your MCU's USB DHCP server **must omit Option 3 (Default Gateway)** so iOS keeps 5G/Wi-Fi online for `[https://gitlab.io](https://gitlab.io)` while routing `mlx.local` (`192.168.73.1`) over USB. *(Tip: Put a tiny 2 KB `<script>` fallback inside `index.html` that renders a basic emergency SCPI console if the `https://` script `onerror` fires in an offline lab.)*
* **Rule 2 — Single Codebase, Dual Entrypoint Across Platforms:**
* **Windows / macOS / Linux / Android:** Open `https://<group>.gitlab.io/<poc>/` directly (`isSecureContext === true`) → app uses **`WebSerial`**.
* **iPhone / iPad:** Open `[http://mlx.local](http://mlx.local)` → loads the exact same JS/CSS from `https://<group>.gitlab.io/<poc>/` → app detects `location.protocol === 'http:'` and uses **`ws://mlx.local/ws`**.


* **Rule 3 — Keep GitLab Pages Public & Build an IIFE Bundle:** Classic `<script src="https://.../app.iife.js">` and `<link rel="stylesheet">` tags load cross-origin even without CORS headers, while `fetch()` for JSON/sensor configs relies on `Access-Control-Allow-Origin: *` (which public GitLab Pages enables by default). Make sure GitLab Pages visibility is set to **Everyone**—if set to *Only Project Members*, GitLab redirects asset requests to `[gitlab.com/oauth/authorize](https://gitlab.com/oauth/authorize)`, which fails cross-origin.
* **Rule 4 — `SecureContext`-Only Browser APIs:** On `[http://mlx.local](http://mlx.local)`, `crypto.subtle` and `navigator.clipboard.writeText()` are disabled by WebKit (`crypto.getRandomValues()` still works). Use a standard hidden `<textarea>` + `document.execCommand('copy')` fallback if your UI has a "Copy Log" button.

---

## 2. Where That "~60-Line DNS Responder" Runs: 100% Inside the MCU

Apologies for the ambiguous wording earlier—it is **not** a Python or shell script on a laptop or phone. It is **~60 lines of C firmware compiled directly into your RP2040 or STM32 binary**, sitting on top of `lwIP`'s raw UDP API (~1 KB of Flash, zero OS threads).

In fact, **TinyUSB already ships this exact C helper in its repository** at `lib/networking/dnserver.c` right alongside `lib/networking/dhserver.c`.

### How It Works Inside the MCU (`lwIP` UDP Port 53)

When the MCU enumerates as a USB Ethernet adapter (`192.168.73.1`), `lwIP` processes every IP/UDP packet coming across the USB cable:

```c
// Runs inside RP2040 / STM32 firmware during lwIP init (no host script, no threads)
struct udp_pcb *dns_pcb = udp_new();
udp_bind(dns_pcb, IP_ADDR_ANY, 53);
udp_recv(dns_pcb, dns_usb_recv_cb, NULL);

```

Whenever iOS sends a UDP Port 53 query over USB, `dns_usb_recv_cb()` inspects the `pbuf`, flips the 16-bit DNS flags from Query (`0x0100`) to Authoritative Response (`0x8400`), appends a fixed 16-byte `A`-record answer (`C0 0C 00 01 00 01 00 00 00 3C 00 04 C0 A8 49 01`, encoding `192.168.73.1`), and calls `udp_sendto()` back over USB in under 0.2 ms.

### Do You Still Need Port 53 (`dnserver.c`) vs. mDNS (`mlx.local`)?

| MCU Discovery Module (Both Run in MCU C Firmware) | Protocol & Port | When iOS Uses It | Which Architecture Needs It |
| --- | --- | --- | --- |
| **`lwIP` mDNS Responder (`mdns_resp_init`)** | Multicast DNS (`UDP 5353`) for **`mlx.local`** | **Always active on USB**, even while iPhone 5G and Corporate Wi-Fi are turned **ON**. | **Your new `[http://mlx.local](http://mlx.local)` Shell + `https://` Cloud Bundle** (Port 53 is *not* needed here). |
| **TinyUSB `dnserver.c` Responder** | Unicast DNS (`UDP 53`) for e.g. `usb.mlx-poc.org` | Used by iOS **only when Wi-Fi/5G are OFF** (Airplane Mode) and USB advertises DHCP Option 6 (DNS). | **Offline `https://` Home-Screen PWA + `WSS**` (where a public CA cert forbids `.local` names). |

---

Toggle between the **`[http://mlx.local](http://mlx.local)` Cloud-Scripted Shell** and the **Offline `https://` PWA + MCU `dnserver.c**` in the diagram below to trace every packet across USB and 5G/Wi-Fi, and click any MCU or browser block to inspect what lives in Flash:

---

> **Why this hybrid `[http://mlx.local](http://mlx.local)` shell is the sweet spot for your RP2040 → STM32F446 path:** As long as having 5G or Wi-Fi coverage on the iPhone is acceptable, you get **unlimited cloud UI size**, **instant GitLab CI web deployments**, **zero TLS/certificate maintenance**, and **~56 KB total comms footprint** on the STM32F446—with an optional 2 KB inline fallback terminal inside `index.html` if someone ever plugs in underground without signal.

Me: But wait, once I have the https installed as an app, that app, can then redirect to http://melexis.local, and melexis.local can then load from the offline PWA? Or not? Offline operation is very important, it's actually the main focus, while also keeping the WiFi and mobile data ON!

**No—redirecting an installed `https://` Home Screen PWA to `[http://melexis.local](http://melexis.local)` cannot load scripts or assets from the offline PWA cache.** Even though both run on the same iPhone, WebKit isolates them behind three independent browser boundaries:

1. **Service Workers Only Intercept Same-Origin Controlled Pages:** Under the Service Worker specification and WebKit's implementation, a Service Worker registered for `[https://group.gitlab.io](https://group.gitlab.io)` only intercepts `fetch` requests made by documents whose own origin is `[https://group.gitlab.io](https://group.gitlab.io)`. Once you redirect to `[http://melexis.local](http://melexis.local)`, that plain-HTTP document is uncontrolled—any `<script src="[https://group.gitlab.io/app.js](https://group.gitlab.io/app.js)">` tag bypasses the PWA's Service Worker completely and attempts a live internet download.
2. **Top-Level Site Cache & Storage Partitioning:** Safari keys both its standard HTTP disk cache and all embedded third-party storage (Cache API, Service Workers, IndexedDB) by `(top-level-site, resource-origin)`. Assets cached when `gitlab.io` is the top-level app live in the `(gitlab.io, gitlab.io)` partition; when `[http://melexis.local](http://melexis.local)` requests those exact same URLs (even via an `<iframe>`), WebKit checks the `(melexis.local, gitlab.io)` partition, which is empty offline. Worse, GitLab Pages serves responses with `Cache-Control: max-age=600` by default, expiring standard HTTP cache entries after 10 minutes.
3. **iOS Standalone PWA Sandbox:** On iOS, Home Screen web apps use an isolated storage container separate from Safari. Navigating out-of-scope to `[http://melexis.local](http://melexis.local)` pops open an overlay in-app browser sheet (with a **Done** button bar) rather than staying inside your full-screen app, and `window.open()` returns `null` inside standalone iOS PWAs.

---

## What "Offline First + Wi-Fi & Mobile Data ON" Forces on iOS

Requiring **offline operation** while **Wi-Fi and Mobile Data remain ON** eliminates every public-DNS trick (`*.usb.melexis.io`, `*.duckdns.org`, and the MCU's unicast UDP port 53 responder):

* To keep Wi-Fi and 5G working normally, your MCU's USB DHCP server **must omit DHCP Option 3 (Default Gateway)**.
* Because Wi-Fi/5G stay active as iOS's primary interfaces, iOS sends all standard unicast DNS queries to the wireless carrier or Wi-Fi router—which fail when offline (or get stripped by corporate DNS-rebinding filters) while ignoring unicast port 53 on the secondary USB interface.
* **Bonjour Multicast DNS (`melexis.local`, UDP port 5353) is the only hostname mechanism on iOS that works 100% offline while Wi-Fi and Mobile Data stay ON.** Apple's `mDNSResponder` broadcasts `.local` queries across every active interface simultaneously—including a gateway-less USB `CDC-ECM` `/30` subnet—regardless of wireless state.

---

## Three Viable Architectures That Meet All Constraints

With `melexis.local` (mDNS + no USB default gateway) handling USB discovery while Wi-Fi and 5G stay ON, and Desktop/Android keeping `WebSerial` on GitLab Pages, three designs fit within the STM32F446's 512 KB Flash limit:

| Criteria | **Option A: Offline `HTTPS` PWA + Private Root CA (`wss://melexis.local`)** | **Option B: Offline `HTTPS` PWA + `HTTP POST` SRAM Bounce (`ws://melexis.local`)** | **Option C: Lean UI in MCU Flash (`[http://melexis.local](http://melexis.local)`) + Cloud `WebSerial**` |
| --- | --- | --- | --- |
| **Works 100% Offline with Wi-Fi & 5G ON?** | **Yes** | **Yes** | **Yes** |
| **Max iPhone UI Size** | **Up to ~50 MB** (stays in `HTTPS` PWA cache) | **~48–64 KB gzipped** (~180 KB unpacked, held briefly in MCU SRAM) | **~120–200 KB gzipped** (stored in STM32 Flash) |
| **STM32F446 Flash Cost (Comms + UI)** | ~100 KB (`lwIP` + mDNS + `mbedTLS`; **0 KB UI**) | **~57 KB** (`lwIP` + mDNS + HTTP bounce; **0 KB TLS, 0 KB UI**) | ~175–255 KB (`lwIP` + mDNS + gzipped UI; **0 KB TLS**) |
| **One-Time iPhone Setup** | Add to Home Screen + **Trust `.mobileconfig` Root CA** | Add to Home Screen only (zero CA profile) | **None** (works cold out of the box) |
| **iPhone Runtime UX** | **True full-screen standalone PWA** (zero redirects, zero URL bar) | Opens iOS in-app browser sheet (**Done** bar at top) | Opens in standard Safari tab (or via NFC tap) |
| **Certificate Lifecycle** | Exempt from 200-day public CA cap; **825-day leaf max** on iOS | **None (never expires)** | **None (never expires)** |

---

### Option A (Recommended): Offline `HTTPS` PWA + Private Root CA (`wss://melexis.local/ws`)

Because public CAs cannot sign `.local` certificates, using **your own Private Root CA** (`mkcert` for the PoC, or an OpenSSL Root CA packaged in a downloadable `.mobileconfig` profile) lets your GitLab Pages `HTTPS` PWA connect directly to **`wss://melexis.local/ws`** offline:

* **Zero Partition Problem:** The user never leaves `[https://group.gitlab.io/poc/](https://group.gitlab.io/poc/)`. Your entire multi-megabyte offline PWA—sensor dashboards, 3D visualizers, cached datasheets, firmware images—stays in full-screen standalone mode and talks directly over `wss://melexis.local/ws` via USB while Wi-Fi and 5G stay ON.
* **One-Time Setup Alongside PWA Install:** When the user visits GitLab Pages online to tap **Add to Home Screen**, they also download `melexis-usb-ca.mobileconfig` and enable full trust under **Settings → General → About → Certificate Trust Settings**.
* **Important iOS 825-Day Rule:** Apple exempts user-installed Root CAs from the 200-day (and upcoming 47-day) public CA limits, so your Root CA profile can last **10–20 years**. However, Apple's `trustd` daemon still rejects any TLS leaf server certificate with a validity period over **825 days (~2.25 years)**, even from a private CA. Mint the MCU's `melexis.local` ECDSA P-256 certificate for **824 days**, and let Desktop `WebSerial` (or a background PWA command) silently push a fresh 824-day leaf cert whenever a board connects. *(Advanced option: put a 10-year Intermediate CA constrained via X.509 `Name Constraints: Permitted DNS:melexis.local` on the MCU so it can self-sign short-lived leaf certs in RAM at boot using the browser's timestamp—eliminating drawer expiry permanently.)*

### Option B (Zero-CA Escape Hatch): Offline `HTTPS` PWA + `HTTP POST → 303` SRAM Bounce

Although `[http://melexis.local](http://melexis.local)` cannot *pull* from the `https://` PWA cache, the `https://` PWA **can *push* a compact self-contained UI bundle from its own offline cache into MCU RAM** over USB:

1. User launches the installed `https://` Home Screen PWA offline (with Wi-Fi and 5G ON) and taps **Connect**.
2. JavaScript reads a pre-built `app-mobile.html.gz` (~48 KB gzipped, ~180 KB unpacked) out of its **own** Service Worker cache (`caches.match()`), Base64-encodes it into a hidden `<form method="POST" action="[http://melexis.local/stage](http://melexis.local/stage)" enctype="text/plain">`, and submits the form.
3. iOS resolves `melexis.local` via USB mDNS, opens the in-app browser sheet, and uploads the 64 KB Base64 payload to the MCU in ~60 ms.
4. The MCU decodes the 48 KB gzipped blob into a **temporary SRAM scratch buffer** (consuming **0 bytes of Flash** and avoiding HTTP/1.1 full-duplex TCP window deadlocks by draining the full `POST` body first), then replies **`303 See Other` → `Location: [http://melexis.local/app](http://melexis.local/app)**`.
5. Safari loads `GET [http://melexis.local/app](http://melexis.local/app)` (`Content-Encoding: gzip`), the MCU frees the 48 KB SRAM buffer back to your sensor application, and the UI runs on origin `[http://melexis.local](http://melexis.local)` over plain **`ws://melexis.local/ws`**—with **zero TLS, zero certificates, and zero iOS profile setup**.

### Option C: Dedicated Lean Mobile UI in MCU Flash (`[http://melexis.local](http://melexis.local)`)

Dropping `mbedTLS` and its key sector frees **~61 KB of Flash** on the STM32F446. If you have ~100–150 KB of spare Flash in your STM32 memory map, compiling a stripped mobile-only Preact + `uPlot` instrument view directly into MCU Flash at `[http://melexis.local](http://melexis.local)` gives instant zero-install offline access on any iPhone (while Desktop/Android use the full GitLab Pages `WebSerial` app).

---

Use the interactive explorer below to compare why the direct `HTTPS → HTTP` redirect hits a WebKit cache-partition miss offline, and step through how **Option A (`wss://melexis.local` + Private Root CA)** and **Option B (`POST → 303` SRAM Bounce)** achieve offline operation while keeping Wi-Fi and 5G active:

---

For your **RP2040 + GitLab Pages PoC**, start by testing **Option A (`wss://melexis.local/ws` with `mkcert`)**: generating `rootCA.pem` and an 824-day `melexis.local` ECDSA certificate with `mkcert` takes two commands, requires zero public DNS setup, keeps your entire multi-megabyte web app inside one clean GitLab Pages codebase (using `WebSerial` on Desktop/Android and `wss://melexis.local/ws` on iPhone), and lets you verify full-screen offline operation with both Wi-Fi and 5G turned ON in an afternoon. If you later decide the one-time iOS Root CA profile step is too much friction for external customers, **Option B's `POST → 303` SRAM bounce** gives you a zero-certificate fallback using the exact same USB `CDC-ECM` + mDNS firmware foundation.

Both `mkcert` commands run **once in a terminal on your development laptop**—never on the iPhone, never on GitLab CI, and never on the RP2040 or STM32. `mkcert` acts as a local certificate factory that outputs static `.pem` files you distribute to the iPhone (public CA file) and the MCU firmware (leaf cert and key).

---

## 1. Running the Two Commands (On Your Laptop)

Install `mkcert` once on your Mac (`brew install mkcert`, or via `apt`/`choco` on Linux/Windows). Before running the two commands, set `CAROOT` to a dedicated folder inside your workspace (kept out of Git) so this PoC's Certificate Authority stays isolated from your personal laptop keychain:

```bash
mkdir -p poc-ca
export CAROOT="$PWD/poc-ca"

# Command 1: Generate the 10-year Private Root CA (rootCA.pem + rootCA-key.pem)
# (Omit -install if you don't need your Mac's own Safari/Chrome to trust it)
mkcert -install

# Command 2: Mint the ECDSA P-256 leaf cert & key for melexis.local
# Note: flags MUST come before the domain/IP list in mkcert
mkcert -ecdsa -cert-file mlx-cert.pem -key-file mlx-key.pem melexis.local 192.168.73.1

```

What those two commands handle automatically under the hood:

* **10-Year Root CA (`Command 1`):** Creates `poc-ca/rootCA.pem` (public root certificate) and `poc-ca/rootCA-key.pem` (CA signing key). Apple exempts user-installed Root CAs from public CA lifetime caps, so the root profile stays valid on the iPhone for 10 years.
* **ECDSA P-256 Key Curve (`-ecdsa`):** Instead of `mkcert`'s default RSA-2048 leaf key, `-ecdsa` generates an **ECDSA P-256 (`prime256v1` / `secp256r1`)** keypair—cutting RP2040 TLS handshake time from over 1.5 seconds down to ~200 ms.
* **Automatic iOS Lifespan Compliance:** `mkcert` hardcodes leaf certificate expiration to **2 years and 3 months (~821 days)** (`time.Now().AddDate(2, 3, 0)`). That keeps the leaf cert just under Apple's hard **825-day maximum** for privately trusted server certificates on iOS without needing a `-days` flag.
* **Proper X.509 SAN Extensions:** Populates `Subject Alternative Name` entries for both `DNS:melexis.local` and `IP:192.168.73.1` alongside `ExtendedKeyUsage = serverAuth`, which iOS `trustd` strictly requires.

---

## 2. Where Each Generated File Goes

Running those two commands produces **four files** on your laptop. Each has one specific destination:

| Generated File | What It Is | Where It Lives | Security Rule |
| --- | --- | --- | --- |
| `poc-ca/rootCA-key.pem` | Root CA Private Key (RSA-3072) | **Stays offline on your laptop** | **Never share or commit.** Anyone holding this key can impersonate websites to devices that trusted `rootCA.pem`. |
| `poc-ca/rootCA.pem` | Root CA Public Certificate (~1.2 KB) | **Installed once on the iPhone** (can be hosted as a download link on your GitLab Pages setup screen) | Safe to distribute publicly—contains no private key material. |
| `mlx-cert.pem` | Signed Server Leaf Certificate (~800 B) | **Flashed onto RP2040 / STM32** | Public certificate sent by the MCU during the `wss://melexis.local/ws` TLS handshake. |
| `mlx-key.pem` | Server ECDSA P-256 Private Key (~240 B) | **Flashed onto RP2040 / STM32** (`certs.h` / `LittleFS`) | Add `*-key.pem` and `certs.h` to `.gitignore` so it never lands in a public GitLab repo. |

> **Why `mkcert`'s mixed RSA/ECDSA split helps the MCU:** `mkcert` uses RSA-3072 for the Root CA and ECDSA P-256 for the `-ecdsa` leaf. During a `wss://` connection, **only the iPhone** checks the RSA-3072 signature on `mlx-cert.pem` (taking under 1 ms on Apple Silicon), while the **RP2040/STM32** never holds `rootCA.pem` and only computes fast ECDSA P-256 signatures using `mlx-key.pem`.

---

## 3. Putting the Files onto the RP2040 and iPhone

### A. On the RP2040 (`arduino-pico` Firmware)

Copy the contents of `mlx-cert.pem` and `mlx-key.pem` into a gitignored `certs.h` header and load them into `BearSSL` at boot:

```cpp
#include <BearSSLHelpers.h>

static const char MLX_CERT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
... paste mlx-cert.pem here ...
-----END CERTIFICATE-----
)EOF";

static const char MLX_KEY[] PROGMEM = R"EOF(
-----BEGIN EC PRIVATE KEY-----
... paste mlx-key.pem here ...
-----END EC PRIVATE KEY-----
)EOF";

BearSSL::X509List serverCert(MLX_CERT);
BearSSL::PrivateKey serverKey(MLX_KEY);
// Pass &serverCert and &serverKey to your BearSSL server / WebSocket listener

```

### B. On the iPhone (One-Time Setup While Online)

When you visit `[https://group.gitlab.io/poc/](https://group.gitlab.io/poc/)` on the iPhone to tap **Add to Home Screen**, install `rootCA.pem` once using Apple's two-step trust flow:

1. **Download the Root CA:** AirDrop `rootCA.pem` from your Mac to the iPhone, or tap a link to `rootCA.pem` hosted directly on your GitLab Pages site in Safari (tap **Allow** on *"This website is trying to download a configuration profile"*).
2. **Install the Profile:** Open iPhone **Settings**, tap **Profile Downloaded** near the top (or go to **Settings → General → VPN & Device Management**), and tap **Install**.
3. **Enable Full Root Trust (Mandatory):** Go to **Settings → General → About → Certificate Trust Settings** (scroll to the very bottom) and toggle **ON** full trust for the `mkcert` root certificate. *(Skipping step 3 is the #1 reason `wss://melexis.local/ws` fails on first test.)*

---

## 4. Moving from `mkcert` (PoC) to Production

`mkcert` is explicitly built for local development and PoCs. Once your RP2040 PoC proves out the offline PWA + `wss://melexis.local/ws` flow with Wi-Fi and 5G left ON, you only change two details for production STM32F446 boards:

* **Per-Board On-Chip Keys (`-csr`):** Even during prototyping, `mkcert` supports signing an MCU-generated Certificate Signing Request via `mkcert -csr board.csr -cert-file mlx-cert.pem` so `mlx-key.pem` never has to exist on a laptop.
* **Domain-Locked Root CA (`Name Constraints`):** `mkcert` generates an unconstrained Root CA (meaning `rootCA-key.pem` could theoretically sign a certificate for any domain). For external customers, generate the Root CA with `openssl` instead, adding X.509 **`Name Constraints: critical, permitted;DNS:melexis.local, permitted;DNS:.melexis.local`** so iOS cryptographically rejects any certificate from that CA unless it is strictly for `melexis.local`.