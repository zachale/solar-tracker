#!/usr/bin/env bash
# Build, flash and capture one bench sketch.
#
# Usage: ./bench/run.sh <sketch-dir> [seconds]
#   ./bench/run.sh 00_card_inventory        # runs until it stops printing
#   ./bench/run.sh 03_sd_stress 600         # capture for 10 minutes
#
# Flashing replaces the tracker firmware on the board. Reflash the real sketch
# when you are done:  arduino-cli compile -u -p <port> --fqbn <fqbn> solar-tracker.ino

set -uo pipefail
BENCH="$(cd "$(dirname "$0")" && pwd)"
SKETCH="${1:?usage: run.sh <sketch-dir> [seconds]}"
SECS="${2:-0}"
FQBN="arduino:renesas_uno:unor4wifi"
DIR="$BENCH/$SKETCH"
[ -d "$DIR" ] || { echo "no such sketch: $DIR" >&2; exit 1; }

PORT="$(arduino-cli board list 2>/dev/null | awk '/unor4wifi/ {print $1; exit}')"
[ -n "$PORT" ] || { echo "No UNO R4 WiFi on USB." >&2; exit 1; }

cp "$BENCH/common/bench_common.h" "$DIR/bench_common.h"
[ -f "$BENCH/../secrets.h" ] && cp "$BENCH/../secrets.h" "$DIR/secrets.h"

echo "==> compiling $SKETCH"
arduino-cli compile --fqbn "$FQBN" "$DIR" 2>&1 | tail -20 || exit 1

echo "==> uploading to $PORT"
arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$DIR" 2>&1 | tail -5 || exit 1

LOG="$BENCH/logs/${SKETCH}-$(date +%Y%m%dT%H%M%S).log"
echo "==> capturing -> $LOG"
sleep 2
stty -f "$PORT" 9600 raw -echo

capture() {
  cat "$PORT" | sed -l -E \
        -e 's/(pass"[[:space:]]*:[[:space:]]*)"[^"]*"/\1"[REDACTED]"/g' \
        -e 's/(password[=:][[:space:]]*)[^&"[:space:]]*/\1[REDACTED]/g' \
    | while IFS= read -r line; do
        printf '%s %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$line"
      done | tee -a "$LOG"
}

if [ "$SECS" -gt 0 ]; then
  capture & CPID=$!
  sleep "$SECS"
  kill "$CPID" 2>/dev/null
  pkill -P "$CPID" 2>/dev/null
else
  capture
fi
echo "==> log: $LOG"
