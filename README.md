# KB2040 Multi-Board Console: USB UART + USB Ethernet + WebSockets + OTA

Each Adafruit KB2040 shows up on the host as **two USB functions at the same time**:

| Function | USB class | Host sees | Used for |
|---|---|---|---|
| UART | CDC-ACM | `/dev/cu.usbmodem*` / `COMx` / `/dev/ttyACM*` | JSON telemetry, text/JSON commands, serial OTA, 1200-baud auto-reset |
| Ethernet | CDC-NCM | a new network adapter that gets `192.168.7.2` (or `192.168.7N.2`) from the board's DHCP server | Web UI, WebSocket push, HTTP API, HTTP OTA, mDNS |

Up to five boards can be connected at once. Each board sits on its own subnet, selected by a **slot** stored in its LittleFS.

**Hosted console (installable, works offline):** <https://karelv.github.io/kb2040-serial-ethernet/>

**Latest firmware (CI build of `main`):** [`firmware.bin`](https://karelv.github.io/kb2040-serial-ethernet/firmware/firmware.bin) · [`firmware.uf2`](https://karelv.github.io/kb2040-serial-ethernet/firmware/firmware.uf2) (for the `RPI-RP2` drive) · [`firmware.json`](https://karelv.github.io/kb2040-serial-ethernet/firmware/firmware.json) (version, commit, size, CRC32)

## Layout

```
.arduino/              project-local arduino-cli, cores, libraries (only arduino-cli.yaml is in git)
firmware/
  firmware.ino         app: identity, slots, LED, WebSocket + HTTP server, OTA engine, Serial
  app_types.h          structs (kept in a header for Arduino's prototype generator)
  dhcp_server.*        minimal DHCP server (leases .2-.9, no router/DNS option)
  flight_log.*         flight recorder: USB/network events of the last 8 boots, kept in LittleFS
  mdns_responder.*     minimal mDNS responder (two names per board, NSEC for AAAA)
  webui.h              GENERATED from web/index.html by compile.sh
  webassets.h          GENERATED from web/ manifest + icons by compile.sh
  build_info.h         GENERATED per build by compile.sh (keeps __DATE__/__TIME__ fresh)
web/                   the console, published to GitHub Pages as a PWA
  index.html           multi-board console (single file; also embedded in and served by every board)
  manifest.webmanifest, icon-*.png, apple-touch-icon.png   PWA manifest + icons (also served by the board)
  sw.js                service worker (offline cache; Pages only, needs HTTPS or localhost)
  vendor/              web-serial-polyfill 1.0.15 (Apache-2.0), so WebUSB serial works offline
tools/make-icons.py    renders the icons
docs/                  research notes: melexis-io-arduino.md (STM32F446 port), iphone-ethernet.md
.github/workflows/pages.yml   builds the firmware and deploys it + web/ to GitHub Pages on push to main
build/                 firmware.ino.bin / .uf2 / .elf / .map after ./compile.sh
compile.sh flash.sh serve.py android-diag.sh
```

## Build and flash

```sh
./compile.sh          # embeds web/index.html, compiles with 4 MB sketch + 4 MB LittleFS into ./build
./flash.sh            # compile, then upload over USB (1200-baud touch) or copy the UF2 to RPI-RP2
./flash.sh /dev/cu.usbmodem2101   # explicit port
NO_COMPILE=1 ./flash.sh           # flash the existing build
```

The FQBN is `rp2040:rp2040:adafruit_kb2040:flash=8388608_4194304`, i.e. 8 MB flash split into 4 MB sketch and 4 MB LittleFS. Override it with `FQBN=...`. LittleFS holds `/slot.txt` and stages OTA images. The first boot after switching from the old "no FS" layout formats it automatically.

If a board doesn't respond, hold **BOOT** while plugging it in and run `./flash.sh` again. It copies the UF2 onto the `RPI-RP2` drive.

### Toolchain bootstrap (already done; repeat on a fresh checkout)

```sh
mkdir -p .arduino/{bin,data,downloads,sketchbook}
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=./.arduino/bin sh
A="./.arduino/bin/arduino-cli --config-file ./.arduino/arduino-cli.yaml"
$A core update-index && $A core install rp2040:rp2040 && $A lib install "Adafruit NeoPixel"
```

Verified with arduino-cli 1.5.1, rp2040 core 6.3.2, and Adafruit NeoPixel 1.15.5. LittleFS, Updater, PicoOTA and Hash ship with the core.

## Open the console

Three ways, all the same `web/index.html`:

| Where | Serial (Web Serial / WebUSB) | USB Ethernet | Offline / install |
|---|---|---|---|
| **GitHub Pages** `https://karelv.github.io/kb2040-serial-ethernet/` | yes (HTTPS) | **not used**: only links that open the board's own page (see below) | yes: service worker + manifest, *Install app* |
| **Board** `http://192.168.7.1/`, `http://kb2040-N.local/` | no (plain HTTP isn't a secure context) | yes | always offline (served by the board); *Add to Home Screen* gets the icon |
| **Local** `python3 serve.py` → `http://localhost:8000/web/` | yes | yes | yes (localhost counts as secure) |

The hosted HTTPS copy deliberately sends **no** `http://` / `ws://` request to a board. Every such request is mixed content (HTTPS page → plain-HTTP device): Safari and Firefox block it, and Chrome allows it only after a *local network* prompt and then shows **Not secure** in the address bar. So on HTTPS the live dashboard, board cards, slots, clone and HTTP updates are hidden, the firmware panel uses Web Serial, and the board bar has buttons that open `http://192.168.7.1/` / `http://192.168.7N.1/` (a normal navigation, so the hosted page keeps its padlock).

### Install as an app

- **Desktop Chrome/Edge, Android Chrome:** open the Pages URL once while online, then use the **Install app** button in the header (or the browser's install icon/menu). After that it starts offline; when online it fetches the latest version.
- **iPhone/iPad:** iOS has no Web Serial/WebUSB and the HTTPS page doesn't talk HTTP to boards, so install from the **board's own page** instead: open `http://192.168.7.1/` in Safari → Share → *Add to Home Screen*. It's served by the board, so it needs no internet.
- Pages is redeployed by `.github/workflows/pages.yml` on every push to `main` that changes `web/`, `firmware/` or `compile.sh`. The workflow builds the firmware with the same pinned toolchain (arduino-cli 1.5.1, rp2040 core 6.3.2, NeoPixel 1.15.5) and publishes it under `firmware/`. Each deploy stamps a new cache version into `sw.js`, so installed apps update (page **and** firmware) on their next online start.

### Local server

```sh
python3 serve.py          # serves the project root with CORS + Private-Network headers
# open http://localhost:8000/web/ in desktop Chrome or Edge
```

`localhost` is a secure context, so Web Serial works there. On the Mac, opening `web/index.html` directly as a `file://` page works as well. The same page is also built into every board at `http://192.168.7.1/`, `http://kb2040-1.local/`, and so on. Those pages work too, except for the serial panel, because plain HTTP isn't a secure context.

The serial panel has two connect buttons:

- **Connect Web Serial** uses Chrome's built-in Web Serial. This is the one to use on desktops.
- **Connect via WebUSB** uses Google's [`web-serial-polyfill`](https://github.com/GoogleChromeLabs/web-serial-polyfill) (v1.0.15, vendored in `web/vendor/`, so it works offline; jsDelivr is the fallback where that folder is missing). It talks to the board's serial interface over WebUSB. This is the one to use on Android, where the page makes it the default. On macOS it fails, because the system's serial driver already owns the interface.

## Multi-board setup

| Slot | Board IP | Host gets | mDNS name |
|---|---|---|---|
| Default (0) | `192.168.7.1` | `192.168.7.2` | `kb2040.local` |
| 1 | `192.168.71.1` | `192.168.71.2` | `kb2040-1.local` (+ alias `kb2040.local`) |
| 2 | `192.168.72.1` | `192.168.72.2` | `kb2040-2.local` (+ alias) |
| 3 | `192.168.73.1` | `192.168.73.2` | `kb2040-3.local` (+ alias) |
| 4 | `192.168.74.1` | `192.168.74.2` | `kb2040-4.local` (+ alias) |

- **Unique identity:** each board reads its flash unique ID (`chipId`, 16 hex digits) and derives a locally administered MAC `02:xx:xx:xx:xx:xx` from it. The host-side MAC is the same with the last byte inverted. So the host sees every board as a separate adapter.
- **No route hijacking:** the DHCP server sends no router and no DNS option, so the host's default route and resolver stay where they were.
- **Add boards one at a time.** Plug in the first board. It comes up on Default, and the console shows an amber prompt: *Assign to Slot 1*. Click it, and the board saves `/slot.txt`, sends a reboot notice with its new URL, and comes back on `192.168.71.1` about 5 s later. The console follows it. Then plug in the next board and repeat. Each board card also has a slot dropdown and an **Identify (Blink LED)** button, which flashes that board's NeoPixel white/cyan 5×.
- **Discovery bar:** the console probes the five addresses and keeps a WebSocket open to each board it finds. Empty addresses are re-probed with backoff: Default at least every 10 s, slot addresses up to every 30 s, and every second where a board is expected back after a reboot. **Rescan** probes everything at once. Chrome logs each refused probe in the DevTools console and a page can't suppress that; tick *Hide network* in the console settings to hide them. Each card shows the slot badge, IP, mDNS name, chip ID, live counter, build and WebSocket status. Click a card to make that board the dashboard's active board.
- **Forgotten slot:** an assigned board also answers `kb2040.local`, and the discovery bar finds it on any slot. If you need a factory reset, use the BOOT button (next section).

### 3-second BOOT reset

While the firmware is running, **hold the BOOT button for 3 s**. The NeoPixel ramps up magenta while you hold it, then flashes rapid magenta. At that point `/slot.txt` is deleted. **Release the button** and the board reboots to Default `192.168.7.1` / `kb2040.local`. It waits for the release because the RP2040 boot ROM would enter RPI-RP2 mode if BOOT were still held during the reset. Holding BOOT while *plugging in* still enters the UF2 bootloader as usual.

### IP collision detection

Two boards on Default means two host adapters on `192.168.7.0/24`. The host routes to only one of them, and the other one is *shadowed*.

- **On the board:** a Default board that is USB-mounted, has the LED off, and hasn't had a single HTTP/WebSocket client in the 6 s since boot **pulses amber every 2 s**. It switches to normal LED behaviour as soon as any client reaches it. Note that a lone Default board with no browser open also pulses amber. The pulse means "nobody has reached me yet", not that a collision is proven.
- **In the browser:** the console remembers which `chipId` answered on each IP. If a different chip answers on the same IP without an announced reboot, a red **IP Collision Detected on 192.168.7.1** banner appears with a one-click *Assign & reboot* to a free slot. If a Default board is online while other boards are connected, the default-board prompt turns red.

## Live dashboard (WebSocket)

`ws://<board>/ws` (RFC 6455, port 80, up to 3 clients per board; a 4th evicts the oldest). Every `broadcastIntervalMs` (default 1000 ms, range 50–10000 ms) the board increments `counter` and pushes this frame to all WebSocket clients and to Serial:

```json
{"type":"telemetry","counter":123,"intervalMs":1000,"led":true,"slot":0,"ip":"192.168.7.1",
 "hostname":"kb2040.local","chipId":"DF60BCA003544E33","mac":"02:5E:44:1F:91:85",
 "version":"Nov  9 2026 11:43:58","uptimeMs":20047,"wsClients":1,"usbRelinks":0}
```

`version` is the firmware build timestamp (`__DATE__ __TIME__`). Commands (WebSocket text frames, `POST /api/cmd`, or a Serial line):

| JSON | HTTP mirror | Serial text | Effect |
|---|---|---|---|
| `{"cmd":"led","state":"on\|off\|toggle"}` | `POST /api/led?state=` | `LED:ON` `LED:OFF` `LED:TOGGLE` | set the NeoPixel, broadcast state |
| `{"cmd":"interval","ms":250}` | `POST /api/interval?ms=` | `INTERVAL:250` | change the push rate (clamped to 50–10000 ms), broadcast |
| `{"cmd":"slot","slot":1}` | `POST /api/slot?slot=` | `SLOT:1` | write `/slot.txt`, broadcast `{"type":"reboot",...,"url":"http://kb2040-1.local/","ipUrl":"http://192.168.71.1/"}`, reboot after 400 ms |
| `{"cmd":"identify"}` | `POST /api/identify` | `IDENTIFY` | blink white/cyan 5× |
| `{"cmd":"state"}` | `GET /api/state` | `STATE` | current telemetry |
| `{"cmd":"reboot"}` | `POST /api/reboot` | `REBOOT` | reboot |
| | | `BOOTSEL` | reboot into the UF2 bootloader |

The dashboard shows the counter, LED, interval slider with 100/250/500/1000/2000 ms presets, measured rate, jitter, missed frames, and the board's identity. Every HTTP response sends `Access-Control-Allow-Origin: *` and `Access-Control-Allow-Private-Network: true`, and `OPTIONS` preflights get a `204`.

## Firmware update: 3 modes, no manual downloads

All modes go through one engine on the board. It works the same way over each transport:

1. It checks the first 256 bytes (boot2) against the running image. This rejects `.uf2` files, random files, and images for other flash chips before anything is written.
2. It streams the image into `firmware.bin` on LittleFS and computes a CRC32 on the fly. The CRC must match the client's `X-CRC32` header or serial argument.
3. It hands the image to PicoOTA and reboots. The core's power-fail-safe OTA bootloader copies the image into flash.

The NeoPixel shows a blue progress animation while this runs. After the reboot, the console's WebSocket reconnects by itself and confirms the build the board now reports.

| Transport | How |
|---|---|
| USB Ethernet | `POST /api/update` with the raw `.bin` as body and optional `X-CRC32: <hex>`. Example: `curl --data-binary @build/firmware.ino.bin -H "X-CRC32: $(python3 -c "import zlib;print('%08X'%zlib.crc32(open('build/firmware.ino.bin','rb').read()))")" http://192.168.7.1/api/update` |
| Web Serial | `OTA:START:<size>:<crc32hex>\n` → `{"phase":"ready","chunk":4096}`, then raw 4096-byte chunks, each answered with `{"phase":"ack","received":N}`, then `{"phase":"done","ok":true}` |

In the console's **Firmware update** panel, pick the transport (*USB Ethernet* targets the active board, *Web Serial* targets the board on the open serial port), then one of:

1. **Stream from URL:** defaults to the **published CI build** (`firmware/firmware.bin` next to the hosted console; the installed app caches it, so this works offline too). The panel shows its version and commit and whether the target already runs it. On `python3 serve.py` it defaults to the local build `http://localhost:8000/build/firmware.ino.bin`. The browser `fetch()`es the image into RAM and streams it to the board. Nothing is saved to disk.
2. **Clone board→board:** with 2+ boards online, *Clone Firmware from Active Board to Target Board* reads the running image from the source board's XIP flash (`GET /api/firmware.bin`, which is byte-identical to `build/firmware.ino.bin` and carries an `X-CRC32` header that the browser checks) and streams it into the target.
3. **Local .bin file:** drag-and-drop or a file picker, with a progress bar, percentage, KB/s and elapsed time.

Measured on macOS with a 229 KB image: curl over Ethernet takes about 4.3 s (≈55 KB/s), Web Serial about 5.8 s (≈40 KB/s), and an in-browser upload 4–13 s depending on Chrome's upload pacing. The board is back on its WebSocket about 3–5 s after staging.

## Phones

The board-to-host link has two paths: USB Ethernet (the board's network) and USB serial. Phones differ in which one they accept.

| | USB Ethernet (dashboard, slots, OTA over HTTP, mDNS) | Serial over WebUSB | Native Web Serial |
|---|---|---|---|
| **Mac / Windows / Linux, Chrome or Edge** | works | not needed (fails on macOS) | works |
| **Android, Pixel 9 Pro (Android 17, Chrome 156), tested** | **works** once the browser has Android's *local network* permission (see below); without it the page fails with `ERR_TOO_MANY_RETRIES` | **works** ("Connect via WebUSB", needs an HTTPS or `localhost` page) | **no**: the port picker lists only Bluetooth devices |
| **Other Android phones, not tested** | depends on the manufacturer's kernel and Android's Ethernet service; expect "no" | expected to work in Chrome on phones that can act as USB host; not in Firefox | only where the manufacturer exposes wired serial ports |
| **iPhone / iPad (USB-C), not tested** | the **only** path; estimated 50–60% chance (see below); use the board's own page, not the HTTPS one | not available (no WebUSB in iOS browsers) | not available |

### Android

- The board powers up and is accepted by the phone. A Default board pulsing amber means the phone has enumerated it, so cable and OTG are fine.
- **The USB-Ethernet link works on the Pixel:** the phone gets `192.168.7.2` from the board (interface `usb0`, Android's Ethernet filter is `(usb|eth)\d+`), the status bar shows `<··>!` (no internet, expected), and ping/HTTP to `192.168.7.1` succeed from `adb shell`.
- **Android 17 blocks browsers from the local network by default.** Chrome then fails with `ERR_LOCAL_NETWORK_PERMISSION_MISSING` (visible in `chrome://net-export`) and shows `ERR_TOO_MANY_RETRIES`. The *Nearby devices* permission does not cover it. Allow the browser's *local network* permission in its app permissions, or over adb:
  ```sh
  P=com.android.chrome   # or com.brave.browser
  adb shell pm clear-permission-flags $P android.permission.ACCESS_LOCAL_NETWORK revoked-compat
  adb shell appops set --uid $P ACCESS_LOCAL_NETWORK allow
  ```
- Test in airplane mode first; with Wi-Fi or mobile data on, Android may route the browser over that network instead. Use the IP rather than `kb2040.local`.
- `./android-diag.sh` collects phone-side logs (interfaces, addresses, ping/HTTP to the board, logcat) without Wi-Fi: start it with the phone on the Mac, move the cable to the board, reproduce, move it back, `./android-diag.sh pull`.
- **With Wi-Fi or mobile data on, use the serial path** (Android sends the browser's traffic over the network that has internet, so the board's network is unreachable then). Open the installed app or <https://karelv.github.io/kb2040-serial-ethernet/>; WebUSB is blocked on plain-HTTP pages such as the board's own page. Tap **Connect via WebUSB** and pick the KB2040: the chooser lists only boards with a USB serial (CDC-ACM) interface. The *Connect Web Serial* button is hidden on Android (it only lists Bluetooth ports there). The polyfill is bundled, so this also works offline.
- That gives you the live counter, LED, identify, interval and slot commands (`SLOT:2`, `INTERVAL:250`, ...), the raw console, and firmware updates with transport **Web Serial** (Android selects it automatically): **Stream from URL** flashes the published build, also offline from the installed app, or use **Local .bin file**.
- The board cards, dashboard, *Stream from URL* and *Clone* need the board network, so on the phone they work only with Wi-Fi and mobile data off.

### iPhone and iPad

All iOS browsers use Safari's engine, which has neither Web Serial nor WebUSB, so USB Ethernet is the only way in. If iOS accepts the board's network, the board's own page (`http://192.168.7.1/` or `http://kb2040-N.local/`) needs nothing beyond plain Safari, so the whole console should work, including firmware updates from a local `.bin` file.

- **For it:** the board uses CDC-NCM, the USB-Ethernet class Apple's own systems use. Developers report an iPad picking up an NCM device automatically, and macOS already works with this board.
- **Against it:** iOS may not like the combined serial + network device, NCM devices have been reported to misbehave after an iPhone sleeps and wakes, and none of this is tested yet.
- **Quick test:** plug a board into the iPhone, look for an **Ethernet** entry in *Settings*, then open `http://192.168.7.1/` in Safari.
- A USB-C iPhone powers the board directly. A Lightning iPhone needs Apple's Lightning to USB Camera Adapter.
- If iOS rejects NCM, the fallback would be ECM, another USB-Ethernet class. The core's USB library doesn't include ECM, so that means changing the board's USB stack.
- Details and test log: [docs/iphone-ethernet.md](docs/iphone-ethernet.md).
- **First test (Nov 2026):** no page. The NeoPixel pulsed amber, so the iPhone powered the board and selected its USB configuration, but no HTTP request arrived. The flight recorder (below) is there to find out which step fails.

### Flight recorder

Each boot gets a record in LittleFS: USB mounts and suspends, which endpoints the host opened, the Ethernet frames received (by type), DHCP messages with the client's host name and vendor class, and the first HTTP request with its User-Agent. The last 8 boots are kept. Saving happens within a second of each milestone, every 10 s for the first 10 minutes, and every 5 minutes after that.

To diagnose a host without a serial console (iPhone, Android): plug the board into that host, try it, then plug the board into a computer and read the log:

- serial: send `FLIGHT` (or `{"cmd":"flight"}`), for example from the console's serial terminal;
- HTTP: `http://192.168.7.1/api/flight`;
- `FLIGHT:CLEAR` (or `{"cmd":"flightclear"}`) empties it.

How to read it: `usb.mounts` > 0 means the host selected the configuration. If the host's network driver started, `usb.epMax` lists 4 IN and 2 OUT endpoints: the NCM data endpoints are only opened when the host selects the data interface. `rx.frames` = 0 means the host never sent a frame. Then come `dhcp` (discover → offer → request → ack) and finally `http`.

## Notes and limits

- lwIP is built with 5 TCP PCBs, so each board allows 3 WebSocket clients and keeps 2 PCBs for HTTP.
- **USB link watchdog:** the core's prebuilt TinyUSB NCM driver sends its "network connected" notification only once per USB reset. In testing, macOS missed it once after a quick reboot and left the adapter at `status: inactive` with no DHCP. The firmware now counts Ethernet frames from the host. If USB is mounted but no frame has arrived within 8 s, it forces a USB re-enumeration, at most 3 times per boot. `usbRelinks` in the telemetry counts these.
- `LED_BUILTIN` on the KB2040 variant points to GPIO 31, which doesn't exist on the chip, so the NeoPixel is the only status LED.
- Edit the UI only in `web/index.html`. `compile.sh` embeds it into `firmware/webui.h`.
