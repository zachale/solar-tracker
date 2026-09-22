#!/usr/bin/env bash
# upload one sketch and capture for N seconds
set -uo pipefail
S="$1"; SECS="$2"
PORT=/dev/cu.usbmodemC04E3011DB182
BENCH="$(cd "$(dirname "$0")" && pwd)"
cp "$BENCH/common/bench_common.h" "$BENCH/$S/"
[ -f "$BENCH/../secrets.h" ] && cp "$BENCH/../secrets.h" "$BENCH/$S/"
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi "$BENCH/$S" >/dev/null 2>&1 || { echo "COMPILE FAIL $S"; exit 1; }
arduino-cli upload -p "$PORT" --fqbn arduino:renesas_uno:unor4wifi "$BENCH/$S" >/dev/null 2>&1 || { echo "UPLOAD FAIL $S"; exit 1; }
/bin/sleep 4
stty -f "$PORT" 9600 raw -echo
L="$BENCH/logs/${S}-$(date +%Y%m%dT%H%M%S).log"
( /bin/cat "$PORT" > "$L" & CP=$!; /bin/sleep "$SECS"; kill $CP 2>/dev/null )
echo "LOG $L $(wc -l < "$L") lines"
