# Melexis.io (STM32F446ME): Arduino board support and the USB Ethernet function

Status: research notes, 2026-11-11. Nothing is built yet. Facts marked *(verify)* come from general STM32F446 knowledge, not from the Melexis.io schematic or a datasheet check.

## 1. Can Melexis.io do what the KB2040 does?

Yes. The KB2040's "Ethernet adapter" is USB **CDC-NCM**: the board only acts like a network card over USB, so the chip needs no Ethernet MAC. The STM32F446 has no Ethernet MAC, and doesn't need one.

| | KB2040 (RP2040) | Melexis.io (STM32F446ME) |
|---|---|---|
| CPU | 2 × Cortex-M0+ 133 MHz (firmware uses only core 0) | 1 × Cortex-M4F 180 MHz |
| USB | Full speed, 12 Mbit/s | Full speed on the built-in PHY, 12 Mbit/s: same throughput |
| RAM | 264 KB | 128 KB |
| Flash | 8 MB external (4 MB sketch + 4 MB LittleFS) | 512 KB internal |
| USB stack | TinyUSB, built into arduino-pico (incl. NCM + lwIP glue) | TinyUSB supports STM32F4 (dwc2 driver) and NCM, but has to be added |
| Recovery | BOOTSEL button, `.uf2` drag-and-drop | ST ROM bootloader, USB DFU (BOOT0 at reset) |

### Current KB2040 firmware size (reference for the port)

- `.bin` 300–310 KB: `.text` 150 KB + `.rodata` 136 KB. That includes the web UI, 68 KB **uncompressed** (about 18 KB gzipped), and the PWA icons/manifest, 56 KB.
- RAM: about 83 KB static (`.data` + `.bss`) plus heap.
- On the F446 the RP2040-specific parts disappear (boot2, PicoOTA, LittleFS). Expected size is roughly 150–250 KB: it fits in 512 KB, but OTA headroom is tight (see §3).

### What carries over unchanged

- The web console (`web/index.html`), the GitHub Pages PWA, and the HTTPS-no-HTTP rule.
  - Only change: add the new USB VID/PID to `USB_ACM_FILTERS` and the Web Serial filters.
- The network code: DHCP server (no router/DNS), mDNS responder, HTTP + WebSocket server, JSON command protocol, slot addressing.
- Host behaviour: Mac/Windows/Linux the same as now.
  - Android has the same limitation: the board is only reachable with Wi-Fi and mobile data off (one default network per app).

### What has to be redone

1. **USB clock.** USB needs exactly 48 MHz. At 180 MHz SYSCLK the main PLL's Q output can't give 48 MHz, so use **PLLSAI_P** through the CK48 mux *(verify in RM0390: RCC_DCKCFGR2.CK48MSEL)*, or run SYSCLK at 168 MHz. An **HSE crystal** is required; the F4 has no crystal-less USB.
2. **Endpoints.** CDC-ACM + CDC-NCM needs EP1..EP4 besides EP0 (ACM notify IN, ACM data IN/OUT, NCM notify IN, NCM data IN/OUT).
   - I believe the F446 OTG_FS core has 6 bidirectional endpoints, which is enough *(verify in RM0390)*.
   - Its FIFO RAM is about 1.25 KB, OK with 64-byte FS packets.
   - Fallback: the OTG_HS core in FS mode (PB14/PB15), only if the board routes USB there.
3. **OTA without LittleFS/PicoOTA.**
   - The F446 flash sectors are 4 × 16 KB, 1 × 64 KB and 3 × 128 KB.
   - Proposal:
     - a small bootloader/updater in sector 0;
     - the app in sectors 1–5 (16+16+16+64+128 = 240 KB);
     - a staging image in sectors 6–7 (256 KB), copied into place by the bootloader after a CRC check.
   - Requires the app to stay ≤ 240 KB, so gzip the web UI and drop or shrink the PWA icons from the firmware.
4. **Board-specific bits:**
   - chip ID: the STM32 96-bit UID at 0x1FFF7A10 *(verify)*, instead of `pico_get_unique_board_id`;
   - status LED instead of the NeoPixel;
   - BOOT0/reset handling instead of BOOTSEL;
   - I²C pins.
5. **Arduino USB.** The STM32duino core's built-in USB (ST middleware) only does CDC (serial). For NCM:
   - Select **USB support: None** (no ST middleware).
   - Add TinyUSB plus lwIP as libraries; the stm32duino `LwIP` library can provide lwIP.
   - Whether Adafruit_TinyUSB_Arduino supports STM32duino needs checking.
   - This is the real porting work, separate from the board definition.

### Single core

The RP2040 firmware runs everything on core 0 (no `setup1()`/`loop1()`). TinyUSB and lwIP are serviced from core-0 interrupts and timers. So the single-core F446 loses nothing.

If strict-timing sensor sampling is needed later:
- **RP2040:** core 1 plus the inter-core FIFO. lwIP stays on core 0, and flash writes pause core 1.
- **F446:** a timer interrupt or DMA instead.

## 2. An Arduino environment for Melexis.io

**Recommendation: yes.** Don't write a core. Write a **board definition on top of STM32duino** (ST's official Arduino core, which already supports the STM32F446). It consists of:
- `boards.txt` entry: name, MCU, upload methods, menus (USB support, optimisation, upload method);
- a **variant**:
  - pin map from the schematic;
  - `SystemClock_Config()` for the board's crystal, with the 48 MHz for USB;
  - default Serial/I²C/SPI pins;
  - LED and button pins;
- a **board-manager package index** (`package_melexis_io_index.json`) that references the STM32duino core, hosted for example on GitHub Pages like the console. Users add one URL in Arduino IDE → Board Manager and select "Melexis.io".

### Flash overhead of Arduino

- Small. An **empty sketch** for a Nucleo-64 board with STM32duino compiled to **10 KB** (`../nucleo64_trial`, 2024 build). USB serial adds roughly another 10–15 KB.
- The real flash users are lwIP (about 40–60 KB, estimate), TinyUSB, the web UI (about 18 KB gzipped) and the application.
- Plain STM32Cube would save perhaps 10–30 KB at the cost of a less approachable environment.

### Why it pays off

- The Melexis sensor work is already Arduino-based (`../mlx90635_hal_arduino.cpp`, the i2c-stick projects, this KB2040 firmware).
- Customers and FAEs could use the Melexis drivers and examples on Melexis.io unchanged.
- CI is the same arduino-cli setup as `.github/workflows/pages.yml`.

### Flashing options

- **ST ROM bootloader, USB DFU** (BOOT0 at reset). The browser can drive it over WebUSB, which is the STM32 equivalent of BOOTSEL/UF2 and works on Android too.
  - On Windows the ROM DFU device needs a driver first (Zadig/WinUSB, or STM32CubeProgrammer's).
  - The F446 ROM bootloader's USB DFU needs HSE; check the supported crystal frequencies in AN2606 *(verify)*.
- ST-Link (SWD).
- Later: the app's own OTA (§1.3).

## 3. What the agent can do alone, and what it needs

**Can do alone:**
- write boards.txt, the variant, the clock setup, the package index and CI;
- compile example sketches;
- with the board on the Mac and flashable (DFU or ST-Link), flash it and test blink, USB serial, I²C scan and USB Ethernet over the serial port / network, as done with the KB2040.

**Needs from the user:**
1. Schematic or pin list: crystal frequency, USB pins (PA11/PA12?), BOOT0/reset wiring, LEDs, buttons, I²C/SPI/UART connectors.
2. How the board is flashed today (ST-Link, DFU, custom bootloader).
3. Physical actions: pressing BOOT0/reset, watching the LEDs.
4. Melexis decisions:
   - USB VID/PID (a Melexis VID or ST's);
   - board/package name;
   - whether and where to publish the board package (outward-facing: ask first);
   - licensing.

## 4. Suggested order

1. **USB proof of concept** (about a day): TinyUSB's `examples/device/net_lwip_webserver` (runs on STM32F4) switched to NCM, plus a CDC-ACM port. Check enumeration on Mac, Windows and Android.
   - A Nucleo-F446RE is the same die in another package, a good place to start before the Melexis.io board.
2. **Board definition:** blink, USB serial and I²C working from the Arduino IDE.
3. **Port the KB2040 firmware:** TinyUSB NCM + lwIP, gzipped web UI, flash-sector OTA.

Open: the user is collecting the Melexis.io hardware details (2026-11-11).
