#!/usr/bin/env bash
# Flash build/firmware.ino.uf2 to an Adafruit KB2040.
#  1. If the board is running (USB serial visible), arduino-cli uploads via 1200-baud touch + picotool/UF2.
#  2. Else, if an RPI-RP2 bootloader drive is mounted, the UF2 is copied onto it.
# Usage: ./flash.sh [serial-port]     (set NO_COMPILE=1 to skip the build step)
set -euo pipefail
cd "$(dirname "$0")"

CLI=./.arduino/bin/arduino-cli
CFG=./.arduino/arduino-cli.yaml
FQBN=${FQBN:-rp2040:rp2040:adafruit_kb2040:flash=8388608_4194304}   # 8 MB = 4 MB sketch + 4 MB LittleFS
BOARD_FQBN=$(echo "$FQBN" | cut -d: -f1-3)   # without menu options, as `board list` reports it
UF2=build/firmware.ino.uf2

[ "${NO_COMPILE:-0}" = 1 ] || ./compile.sh
[ -f "$UF2" ] || { echo "$UF2 not found - run ./compile.sh"; exit 1; }

PORT=${1:-}
if [ -z "$PORT" ]; then
  # Prefer a port arduino-cli identifies as the KB2040, else any RP2040/Adafruit USB serial port.
  PORT=$("$CLI" --config-file "$CFG" board list --format json 2>/dev/null | python3 -c '
import json, sys
try: data = json.load(sys.stdin)
except Exception: sys.exit(0)
ports = data.get("detected_ports", data) if isinstance(data, dict) else data
best = ""
for p in ports or []:
    port = p.get("port", {}); props = port.get("properties", {}) or {}
    boards = p.get("matching_boards", []) or []
    vid = (props.get("vid") or "").lower()
    if any(b.get("fqbn") == "'"$BOARD_FQBN"'" for b in boards): print(port.get("address","")); sys.exit(0)
    if vid in ("0x239a", "0x2e8a") and not best: best = port.get("address","")
print(best)
')
fi

if [ -n "$PORT" ]; then
  echo "Uploading via $PORT ..."
  exec "$CLI" --config-file "$CFG" upload --fqbn "$FQBN" -p "$PORT" --input-dir ./build ./firmware
fi

for VOL in /Volumes/RPI-RP2 /media/"$USER"/RPI-RP2 /run/media/"$USER"/RPI-RP2; do
  if [ -d "$VOL" ]; then
    echo "Copying $UF2 to $VOL ..."
    cp "$UF2" "$VOL"/ && sync
    echo "Done - the board reboots into the new firmware."
    exit 0
  fi
done

echo "No KB2040 found. Plug it in (or hold BOOT while plugging in to get the RPI-RP2 drive) and retry."
exit 2
