# USB Ethernet on iPhone

Status:
- 2026-11-11. First test failed; diagnosis in progress with the flight recorder.
- 2026-11-12. Researched in an AI chat ([iphone-research.md](iphone-research.md)). Requirements clarified, conclusion and test plan below; nothing of the plan is executed yet.

## Requirements (2026-11-12)

| Platform | How the console talks to the board | Ethernet |
|---|---|---|
| Windows, macOS, Linux | HTTPS console (GitHub Pages) or its installed PWA, over **Web Serial** | nice to have; **no error in Device Manager** on Windows |
| Android | the same HTTPS console / PWA, over **WebUSB** (the Web Serial polyfill; native Web Serial only lists Bluetooth ports on Android) | nice to have |
| iPhone / iPad | the **board's own page** over **USB Ethernet**, installed to the Home Screen | **required**, with Wi-Fi and mobile data **ON**, and also offline / in airplane mode |

- **No TLS** anywhere on the board: no certificates, no public DNS, no root CA profile on the phone.
- **The whole iPhone site is served from the MCU** (HTML, JS, CSS, icons, manifest). Nothing it needs comes from the internet. No `https://` assets. For the PoC also no IndexedDB loader; that's parked for when the UI grows (see Conclusion §3).

## Conclusion (2026-11-12)

1. **The architecture is already right.**
   - With Wi-Fi/data ON and no internet dependency, the only working combination on iOS is: a USB network link **without a default gateway**, plus a page **served by the board itself** over plain `http://`, plus a same-origin `ws://` WebSocket. The research chat reaches the same conclusion (its "Option C").
   - That is exactly what the firmware does today:
     - the DHCP server sends no router (option 3) and no DNS (option 6) (`firmware/dhcp_server.cpp`), so iOS should keep Wi-Fi/cellular as its primary network and route only `192.168.7.0/24` over USB;
     - the whole console (`web/index.html`, 68 KB, about 18 KB gzipped, plus PWA icons and manifest) is served from the board, so it works with no internet at all;
     - `kb2040.local` comes from the board's mDNS responder, which is the only name lookup iOS sends over a gateway-less link.
   - *Add to Home Screen* from `http://192.168.7.1/` gives an app icon that opens the board's page.
     - It has no service worker (plain HTTP isn't a secure context), and needs none: the board is the server.
     - It only opens while a board is plugged in.
   - The HTTPS console stays serial-only (Web Serial / WebUSB). On iOS it can't talk to the board, by design: no `http://`/`ws://` from the HTTPS page.
2. **Why the site must come from the MCU, not from the HTTPS PWA's offline cache:**
   - a service worker only serves pages from its own site (`https://karelv.github.io`), never `http://192.168.7.1`;
   - a plain `http://` page can't register a service worker of its own;
   - Safari partitions its cache by top-level site, so files cached for GitHub Pages don't count for the board's page (and GitHub Pages sends `max-age=600`).
   - **Consequence for Melexis.io:** the gzipped UI must fit in the STM32F446 flash budget, next to the app and the OTA staging area (see [melexis-io-arduino.md](melexis-io-arduino.md) §3). At about 18 KB gzipped it does. If the UI outgrows it, add the IndexedDB loader (§3).
3. **Rejected or parked from the research chat:**
   - *Public DNS name → `192.168.73.1` + public CA certificate* (`wss://local.usb.melexis.io`). Needs internet for DNS, breaks on corporate Wi-Fi with DNS-rebinding protection, and certificates expire after 200 days (100 days from March 2027). Rejected.
   - *MCU DNS server on UDP 53 (`dnserver.c`) + DHCP option 3/6.* Only used by iOS when Wi-Fi/data are OFF, and option 3 takes the internet away from the phone. Rejected.
   - *`http://` shell on the board that loads the app from `https://` Pages.* Doesn't work offline. Rejected.
   - *Loader on the board that stores the app from Pages in the board origin's IndexedDB.* **Parked for after the PoC**: the way out if the web UI outgrows the MCU flash (likely on the STM32F446). How it would work:
     - the MCU serves a small loader page plus a basic built-in console;
     - when the phone is online, the loader fetches the full app from GitHub Pages over HTTPS (allowed from an `http://` page; GitHub Pages sends CORS `*` *(verify)*) and stores it in IndexedDB for the board's address;
     - offline, it runs the stored copy. Still no TLS on the board.
     - Costs:
       - a first online visit per phone and per board address (`192.168.7.1`, `192.168.7N.1` and `kb2040.local` each count as a separate site);
       - Safari's 7-day storage eviction (Home Screen apps have their own usage counter, so launch from the icon);
       - checking that the stored app matches the board's firmware version.
     - Not for the PoC: serve everything from the MCU first.
   - *HTTPS PWA + private root CA + `wss://kb2040.local`* (research "Option A"). Needs TLS on the board and a trusted root CA profile on every iPhone. Rejected (no TLS).
   - *PWA pushes a UI bundle into MCU RAM by `POST` → `303`* (research "Option B"). Not needed while the UI fits in flash. Rejected.
4. **The open problem is lower down: the USB network link never came up on the iPhone.**
   - The first test showed USB enumeration (the board was powered and configured) but no HTTP request. The board is CDC-NCM + CDC-ACM in one configuration.
   - Main hypothesis: iOS doesn't start a network driver for this device. Either it doesn't accept CDC-NCM from it, or the composite with CDC-ACM is the problem. The research chat says Apple's CDC-ECM driver is the reliable one on iOS, and that TinyUSB NCM is flaky on iOS 26 *(not verified: AI chat claim, no source)*.
   - Second-order risks, only after the link is up: DHCP, routing with Wi-Fi ON, and Safari specifics.
5. **Choose the USB network class for iOS alone.** Ethernet on the other platforms is only nice to have, so no switching or fallback scheme is needed. One fixed USB configuration: **CDC-ACM (serial, for everyone else) + the network class iOS accepts.**
   - **If iOS accepts NCM** (with ACM next to it): keep the current descriptors. Windows 10 (2004+)/11 has a built-in NCM driver (`UsbNcm`) *(verify)*, so there is no Device Manager error, and Ethernet works on all hosts as a bonus.
   - **If iOS needs ECM:** use ACM + ECM.
     - Windows has **no CDC-ECM driver** *(verify)* and would show the ECM function with a warning.
     - Fix: a **Microsoft OS 2.0 descriptor** (BOS descriptor + vendor request; TinyUSB supports it, as in its `webusb_serial` example) that gives the ECM function the compatible ID `WINUSB`. Windows then binds its generic WinUSB driver: no warning, no Ethernet on Windows (acceptable).
     - The ACM function is unaffected, so Web Serial keeps working. The Android WebUSB chooser filters on CDC-ACM, so it still lists the board.
     - macOS (`AppleUSBECM`) and Linux (`cdc_ether`) have ECM drivers built in, so Ethernet keeps working there as a bonus.
   - **If iOS only works without ACM** (NCM-only or ECM-only): that conflicts with serial for the other platforms. Only then consider two USB configurations or a re-enumeration fallback; see Phase 1.
   - Port to Melexis.io (STM32F446): ECM uses the same endpoints as NCM (notify IN, bulk IN, bulk OUT), so the endpoint/FIFO budget doesn't change. TinyUSB has ECM for the dwc2 core.
6. **Confidence:** the browser/network side is high, because the design needs no internet, no DNS and no certificates. The USB class question is the real unknown, and only measurement settles it. The README's "50–60 %" estimate stands until Phase 1 below has been run.

## Why USB Ethernet is the only path on iOS

- Every iOS browser uses Safari's engine (WebKit). It has neither Web Serial nor WebUSB, so the serial console (Web Serial or the WebUSB polyfill) is not available.
- The hosted HTTPS console (GitHub Pages) deliberately makes no `http://`/`ws://` requests to boards (no "Not secure" badge). On iOS the console therefore has to be the **board's own page**: `http://192.168.7.1/` (or `http://192.168.7N.1/`, `http://kb2040-N.local/`) in Safari.
- It can also be installed from there: Share → *Add to Home Screen*.
- **USB-C iPhones** power the board directly. **Lightning iPhones** need Apple's Lightning to USB Camera Adapter, which supplies limited power *(verify; the research chat says ≤ 100 mA)*.
- The board asks for 250 mA in its configuration descriptor (`USBD_MAX_POWER_MA=250`, arduino-pico `boards.txt`). The first test's mount shows a USB-C iPhone accepts that. For Lightning tests, build with 100 mA.

### Safari, Chrome or Firefox on iOS

**Use Safari.** Chrome and Firefox on iOS are not easier, only slightly harder:
- **Same engine.** On iOS they are Safari's engine (WebKit) with a different interface. So there is no Web Serial or WebUSB in them either, the same mixed-content rules apply, and they can't open the hosted HTTPS console's serial path.
  - Since iOS 17.4, apps in the EU may ship their own browser engine. Neither Chrome (Blink) nor Firefox (Gecko) has shipped one for iPhone users *(verify as of late 2026)*. Even then it would only apply in the EU, so it can't be relied on.
- **Extra permission.** iOS's *Local Network* privacy applies to third-party apps. Chrome/Firefox ask for it on the first connection to `192.168.7.1`; if it's denied, the page fails until *Settings → Chrome → Local Network* is switched on. Safari is exempt.
- **Home Screen app.** *Add to Home Screen* also works from Chrome on iOS (16.4+), but the icon opens in Apple's web-app runtime, the same as from Safari. So the browser used to install it doesn't matter afterwards.

## First test (2026-11-11): failed

- Did not work, with Wi-Fi on, nor in airplane mode with Wi-Fi off.
- **The NeoPixel pulsed amber.** In the firmware that is `collisionWarning()` (`firmware/firmware.ino`): slot 0, no HTTP/WS contact yet, more than 6 s after boot, and `eth.isLinked()` true.
  - `isLinked()` is `tud_mounted()`, so the iPhone **powered the board and selected its USB configuration**.
  - Power and enumeration are fine.
  - **No HTTP request reached the board.**

### Where it can break (still open)

1. **iOS doesn't start a network driver for CDC-NCM.** The host sends no frames.
   - The firmware's USB link watchdog then re-enumerates after 8 s, at most 3 times; the LED goes dark briefly.
   - I'm not sure iOS supports NCM. It does support **CDC-ECM** (for example the Raspberry Pi gadget-mode Ethernet that people use with iPads).
   - The combined serial + network device might also be a problem; iOS has no CDC-ACM driver.
2. **The link comes up but no address:** DHCP doesn't complete, or iOS ignores a link without a router.
3. **There is an address but the browser doesn't reach the board.** Chrome/Brave/Firefox on iOS need *Settings → [browser] → Local Network* on; Safari doesn't. iOS might also route the board's subnet elsewhere while Wi-Fi is up.

## Test plan

Run it in phases; each phase only starts when the previous one passes. Every iPhone run ends the same way: leave the board plugged in for at least 20 s more, then plug it into the Mac and read `FLIGHT`. Note the result in the [test log](#test-log) below.

### Setup

- Record once: iPhone model and connector (USB-C, or Lightning + Camera Adapter), iOS version, and whether iCloud Private Relay is on.
- Board 1 (chipId `DF60BCA003544E33`) in slot 0 (`192.168.7.1`), so results compare with the Mac reference session below.
- `FLIGHT:CLEAR` before each run, so each boot in the log is one run.
- Safari for all phases; Chrome on iOS only in Phase 3.
- Run the Mac reference session again with every new build, before plugging it into the iPhone.

### Phase 0: read what we already have (no new firmware)

1. Plug board 1 (current build, ACM + NCM) into the iPhone in **airplane mode, Wi-Fi off**. Wait 30 s.
2. Check *Settings*: is there an **Ethernet** row? If so, tap it: what IP address (`192.168.7.2`, `169.254.x.x`, none)?
3. Safari: `http://192.168.7.1/`. Note the exact error.
4. Back on the Mac: `FLIGHT`. Read:
   - `usb.epMax`: **4 IN + 2 OUT** means iOS started the NCM data interface; **3 IN + 1 OUT** means it didn't;
   - frames received (ARP, IPv6, mDNS), DHCP messages (DHCP vendor class `60`, host name `12`), relinks.
5. The result picks the branch:

| Flight log shows | Meaning | Go to |
|---|---|---|
| 3 IN + 1 OUT, no frames, no Ethernet row | iOS didn't start a network driver | Phase 1 |
| 4 IN + 2 OUT, frames, no DHCP ack, or Ethernet row with `169.254.x.x` | link up, addressing fails | Phase 2a |
| DHCP ack, Ethernet row with `192.168.7.2`, no HTTP | addressing OK, browser/routing fails | Phase 3 |
| HTTP requests logged | it works in airplane mode | Phase 2b (Wi-Fi/data ON) |

### Phase 1: find a USB class that iOS accepts (airplane mode, Wi-Fi off)

Change one thing at a time. Each build is checked on the Mac first. Builds with ACM are preferred, because the other platforms need serial.

| Build | USB functions | Effort | Answers |
|---|---|---|---|
| B1 | ACM + NCM (current) | none | baseline (Phase 0) |
| B2 | NCM only (`NCM_TEST=1 ./compile.sh`, `FW_NCM_TEST`) | none, exists | is the composite with ACM the problem? |
| B3 | ACM + ECM | medium: ECM driver + lwIP glue (see below) | does iOS want ECM? **target build if B1 fails** |
| B4 | ECM only | small once B3 exists | only if B3 fails: is ACM the problem with ECM too? |

- Stop at the first build that gets an Ethernet row and frames; note its exact class set.
- If only an ACM-less build (B2 or B4) works, ACM and the iOS link don't go together. Options then, in order:
  - two USB configurations (1 = ACM + network class for the desktops, 2 = network class only) and test which one iOS selects;
  - a re-enumeration fallback: start with ACM, and if the host never starts the network interface within ~5 s and the serial port isn't open, re-enumerate once without ACM.
- **Building ECM on the RP2040.** arduino-pico ships TinyUSB precompiled with NCM, and its lwIP glue (`NCMEthernetlwIP`) only knows NCM. Two ways:
  - switch the core's USB stack menu to **Adafruit TinyUSB** (its copy has `ecm_rndis_device.c`, enabled with `CFG_TUD_ECM_RNDIS`), and write an `ECMEthernet` driver class with the same interface as the NCM one for `LwipIntfDev`; or
  - rebuild the core's TinyUSB library with ECM enabled.
  - Either way, mDNS, DHCP server, HTTP/WS and the flight log stay as they are; only the frame in/out path changes.
  - Add the MS OS 2.0 descriptor (WinUSB on the ECM function) in the same build, so Phase 4 can check Windows right away.
- **Optional shortcut before writing ECM firmware:** a Raspberry Pi Zero/4 in USB gadget mode (configfs) can switch between NCM, ECM, ACM + NCM and ACM + ECM in minutes, with `dnsmasq` handing out an address without a router. Plug it into the iPhone; whichever function gets an Ethernet row tells us what to build.
- **Useful extra in the flight log** (if the TinyUSB build allows it): the control requests during enumeration (SET_CONFIGURATION, SET_INTERFACE, NCM `GET_NTB_PARAMETERS`).
  - If iOS sends `GET_NTB_PARAMETERS` but never sets alternate setting 1, its NCM driver did load but gave up.
  - If iOS sends nothing class-specific at all, no driver bound to the interface.

### Phase 2: addressing and routing

**2a. DHCP (airplane mode, Wi-Fi off).** Only if the link comes up but there is no address.
- Log the iPhone's DHCP parameter request list (option 55) and whether it sets the broadcast flag.
- Diagnostic build that sends option 3 (router `192.168.7.1`). **Only to find out whether iOS ignores a link without a router.** It's not for release: with a router, iOS may route all traffic over USB and the phone loses its internet.
- If iOS needs a router to bring the link up, try DHCP option 121 (classless static route) for `192.168.7.0/24` on-link before thinking further.

**2b. Wi-Fi and mobile data ON, and offline (the real requirement).** Run with the first build that worked in airplane mode.
- Cases, each with Safari on `http://192.168.7.1/`:
  1. Wi-Fi ON, mobile data OFF;
  2. Wi-Fi OFF, mobile data ON;
  3. both ON;
  4. both ON but no internet (Wi-Fi network without uplink, or a lift/basement);
  5. airplane mode (Wi-Fi and data OFF): the baseline from Phase 0/1, repeat it with the final build.
- In cases 1–3 also check:
  - another tab still loads a public site (the phone's internet is unaffected);
  - *Settings → Ethernet* still shows `192.168.7.2`;
  - `http://kb2040.local/` resolves (mDNS over USB while Wi-Fi is on);
  - repeat with iCloud Private Relay on and off.
- Office Wi-Fi and home Wi-Fi. If a Wi-Fi network uses `192.168.7.0/24` itself, the subnets collide; then use another slot (`192.168.7N.1`).

### Phase 3: browser and app behaviour (Wi-Fi/data ON, then airplane mode)

- **Safari tab:** page loads, live counter over the WebSocket, LED/identify/interval commands, slot change.
  - Test the WebSocket by IP and by name (`ws://192.168.7.1/ws` and `ws://kb2040.local/ws`). The research chat says IP-literal WebSockets can fail on iOS 26 *(not verified)*.
- **Nothing loaded from the internet:** in airplane mode the page must look and work the same as online (no missing fonts, icons or scripts). The Safari Web Inspector (Mac → Develop menu → the iPhone) shows every request; all must go to the board.
- **HTTPS upgrade:** check that Safari doesn't hang trying `https://192.168.7.1` first. The board's lwIP should refuse port 443 at once.
- **Home Screen app:** *Add to Home Screen* from the board's page. Then:
  - launch it with the board plugged in, with Wi-Fi/data ON and also in airplane mode;
  - launch it with the board unplugged: note what iOS shows (the board is the server, so a "can't connect" page is expected);
  - check that the icon and name come from the board's manifest / `apple-touch-icon`.
- **Chrome on iOS:** the *Local Network* prompt appears on the first visit, and the page works once allowed. Note what happens when it is denied.
- **Firmware update** from a local `.bin` file via the board's page (HTTP transport), on the phone.
- **Robustness:**
  - unplug/replug 10 times in a row: the page is back within 10 s each time;
  - lock the phone 5 min and unlock: the WebSocket reconnects (NCM was reported to misbehave after sleep/wake);
  - leave the phone on the board for 30 min;
  - two boards via a USB-C hub, if the iPhone allows it (slots 1/2, `kb2040-N.local`).

### Phase 4: regression on the other platforms (with the chosen build)

The must-haves there are serial and a clean Device Manager; Ethernet is only noted.

| Host | Must work | Nice to have (note only) |
|---|---|---|
| Windows 11 (and Windows 10 ≥ 2004) | Web Serial on the COM port (HTTPS console and installed PWA), serial OTA; **no warning icon in Device Manager** | Ethernet (only with NCM) |
| macOS | Web Serial, serial OTA | Ethernet |
| Linux (Ubuntu, Chrome) | Web Serial (`/dev/ttyACM*`), serial OTA | Ethernet |
| Android, Pixel 9 Pro | WebUSB serial with Wi-Fi/data ON and in airplane mode; the chooser lists the board (CDC-ACM present) | Ethernet in airplane mode |

Also check that the HTTPS console still makes no `http://`/`ws://` requests (`performance.getEntriesByType('resource')`).

### Phase 5: decide and carry over

- Write down the USB class set iOS accepts; update the README platform table (remove the "50–60 %" estimate).
- **Melexis.io (STM32F446):** same descriptors with TinyUSB (dwc2), including the MS OS 2.0 descriptor if ECM is chosen. Check that the ECM driver, its buffers and the gzipped UI fit the F446 RAM/flash budget in [melexis-io-arduino.md](melexis-io-arduino.md).

### Done when

On a USB-C iPhone, in Safari:
- with **Wi-Fi and mobile data ON**, and also in **airplane mode**, Safari or the Home Screen icon opens the board's console at `http://192.168.7.1/` within 10 s of plugging in;
- it shows live data over the WebSocket and can update firmware;
- no request leaves the board (works identically offline);
- the phone's internet keeps working at the same time;
- it survives 10 replugs and a 5-min lock;

and the same build passes Phase 4: serial on Windows, macOS, Linux and Android, and no Device Manager warning on Windows.

## Flight recorder (built 2026-11-11, not committed yet)

`firmware/flight_log.*` records per boot, in LittleFS (last 8 boots):
- USB mounts, unmounts, suspends and relinks;
- endpoints enabled at mount and the maximum afterwards;
- frames received from the host, by type (ARP, IPv4, IPv6, ICMP echo, mDNS, TCP SYN to port 80);
- DHCP messages in each direction, with the client MAC, host name (option 12) and vendor class (option 60);
- HTTP requests, WebSocket connects, the first request and its User-Agent.

It saves within a second of each milestone, every 10 s for the first 10 minutes, and every 5 min afterwards.

**Read it** on a computer: serial `FLIGHT` / `{"cmd":"flight"}`, or `GET http://192.168.7.1/api/flight`. Clear it with `FLIGHT:CLEAR`.

**Key signal:**
- The NCM data endpoints only open when the host selects the data interface's alternate setting 1, i.e. when the host's network driver actually starts.
- With the driver running, `usb.epMax` shows **4 IN + 2 OUT** endpoints.
- Without it, expect 3 IN + 1 OUT: ACM notify + data, NCM notify.

**To add for the plan:** DHCP option 55 and the broadcast flag (Phase 2a), the USB class set per boot (Phase 1), and the enumeration control requests if feasible (Phase 1).

### Mac reference session (board 1, chipId DF60BCA003544E33, firmware "Nov 11 2026 14:55:28")

| step | result |
|---|---|
| USB mount | at 0.27 s |
| endpoints | IN 1,2,3,4 / OUT 1,2 at mount (the driver started right away) |
| first frame | 0.6 s |
| DHCP | discover 0.7 s → offer → request → ack 1.7 s, lease .2 |
| traffic mix (first 50 s) | 87 frames: 13 ARP, 35 IPv4 (23 mDNS), 39 IPv6 |
| HTTP | `curl` requests arrived |

## Test log

| date | phase | build | iPhone / iOS | Wi-Fi / data | Ethernet row / IP | flight log | result |
|---|---|---|---|---|---|---|---|
| 2026-11-11 | first test | B1 (ACM + NCM) | ? | on, then airplane | ? | not recorded yet | no page; LED amber (mounted, no HTTP) |

## Research chat notes

The full chat is in [iphone-research.md](iphone-research.md). What to keep in mind when reading it:
- It started from "the `https://` site must talk to the board". With the HTTPS-no-HTTP rule and the board-served page, that premise is gone, and with it all of its TLS/ACME/DNS machinery.
- **Taken over:**
  - no DHCP option 3/6, so Wi-Fi/data stay primary;
  - mDNS is the only offline name lookup with Wi-Fi ON;
  - ECM as the likely iOS class;
  - a service worker can't serve an `http://` page, so the iPhone UI lives in MCU flash;
  - Windows has no ECM driver.
- **Contradicted by our own tests:** "Android Chrome 148+ has native Web Serial". On the Pixel 9 Pro (Chrome 156) the Web Serial picker lists only Bluetooth ports; WebUSB is the Android serial path.
- **Unverified claims, treat as hypotheses:**
  - NCM DHCP flakiness on iOS 26;
  - IP-literal WebSocket failures on iOS 26;
  - the 100 mA limit on the Lightning adapter;
  - the exact USB FIFO split of the F446;
  - the Flash/RAM figures for mbedTLS.
