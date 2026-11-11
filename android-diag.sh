#!/usr/bin/env bash
# Android diagnostics for the board's USB-Ethernet link, without Wi-Fi.
# The phone has one USB port, so: start a logger on the phone over USB from the Mac, move the
# phone's cable to the board, reproduce, move it back, pull the logs.
#
#   ./android-diag.sh start [seconds]   # default 600 s; the logger stops by itself after that
#   ./android-diag.sh pull              # stop the logger and copy everything to ./diag/<time>/
set -euo pipefail
cd "$(dirname "$0")"
D=/data/local/tmp/kbdiag

case "${1:-}" in
start)
  DUR=${2:-600}
  adb get-state >/dev/null
  tmp=$(mktemp)
  cat > "$tmp" <<'PHONE'
#!/system/bin/sh
# Runs on the phone. Every 2 s: interfaces, IPv4 addresses, and ping + HTTP to <subnet>.1 for
# every IPv4 address the phone holds (the board is always .1). Full snapshot on every change.
D=/data/local/tmp/kbdiag
END=$(( $(date +%s) + $1 ))
logcat -v threadtime -b main -b system -b events -b crash -f $D/logcat.txt -r 4096 -n 8 &
echo $! > $D/logcat.pid

snap() {
  f=$D/snap-$(date +%H%M%S)-$1.txt
  {
    echo "=== $(date '+%H:%M:%S') $1"
    echo "--- ip addr";  ip addr
    echo "--- ip rule";  ip rule
    echo "--- routes";   ip route show table all
    echo "--- arp";      cat /proc/net/arp
    echo "--- ethernet"; dumpsys ethernet
    echo "--- usb";      dumpsys usb
  } > $f 2>&1
  dumpsys connectivity > $D/conn-$(date +%H%M%S)-$1.txt 2>&1
}

prev=""; i=0
snap start
while [ $(date +%s) -lt $END ]; do
  ts=$(date '+%H:%M:%S')
  ifs=$(ls /sys/class/net | tr '\n' ' ')
  v4=$(ip -o -4 addr show 2>&1 | grep -v ' lo ' | sed -E 's/^[0-9]+: ([^ ]+) +inet ([^ ]+).*/\1=\2/' | tr '\n' ' ')
  probes=""
  for a in $v4; do
    ip=${a#*=}; ip=${ip%/*}; t=${ip%.*}.1
    p=$(ping -c 1 -W 1 $t >/dev/null 2>&1 && echo ok || echo fail)
    h=$( (printf 'GET / HTTP/1.0\r\n\r\n' | timeout 3 nc $t 80 | head -1 | tr -d '\r') 2>&1 )
    probes="$probes $t:ping=$p,http=[$h]"
  done
  echo "$ts ifs=[$ifs] v4=[$v4]$probes" >> $D/loop.txt
  state="$ifs|$v4"
  if [ "$state" != "$prev" ]; then [ $i -gt 0 ] && snap change; prev=$state; fi
  i=$((i + 1))
  sleep 2
done
snap final
kill $(cat $D/logcat.pid) 2>/dev/null
PHONE
  adb shell "rm -rf $D; mkdir -p $D"
  adb push "$tmp" $D/run.sh >/dev/null
  rm -f "$tmp"
  adb shell "setsid nohup sh $D/run.sh $DUR > $D/run.out 2>&1 < /dev/null &"
  sleep 3
  adb shell "tail -n 1 $D/loop.txt" || { echo "logger did not start, see: adb shell cat $D/run.out"; exit 1; }
  echo "Logger running for $DUR s. Now unplug the Mac and plug in the board."
  ;;
pull)
  out=diag/$(date +%Y%m%d-%H%M%S)
  mkdir -p "$out"
  adb shell "pkill -f $D/run.sh; kill \$(cat $D/logcat.pid) 2>/dev/null" || true
  adb pull $D/. "$out/" >/dev/null
  adb pull /sdcard/Download/chrome-net-export-log.json "$out/" >/dev/null 2>&1 || true
  echo "Saved to $out:"; ls "$out"
  echo "--- loop.txt (last 40 lines)"; tail -n 40 "$out/loop.txt"
  ;;
*)
  sed -n '2,8p' "$0"; exit 1 ;;
esac
