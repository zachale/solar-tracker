#!/usr/bin/env bash
# Capture serial output from the solar tracker into a searchable, timestamped log.
#
# The firmware prints a status block every ten seconds (wind, temperature,
# extension percent, RTC time, button states, tracker status) plus one-off
# messages during boot, WiFi association and actuator moves. Every line gets a
# host timestamp prefix so entries stay correlatable even when the RTC is wrong.
#
# Usage:  ./docs/diagnostics/capture-serial.sh [port]
# Stop with ctrl-c. Safe to re-run; output appends to a per-day file.

set -uo pipefail

PORT="${1:-$(arduino-cli board list 2>/dev/null | awk '/unor4wifi/ {print $1; exit}')}"
if [ -z "$PORT" ]; then
  echo "No Arduino UNO R4 WiFi found on USB. Plug it in and retry." >&2
  exit 1
fi

LOG="$(cd "$(dirname "$0")" && pwd)/serial-$(date +%Y-%m-%d).log"
echo "capturing $PORT -> $LOG"
echo "ctrl-c to stop"

stty -f "$PORT" 9600 raw -echo

# The dashboard posts WiFi passwords as form parameters, and the firmware echoes
# every parameter it acts on. Redact them on the way in so they never reach disk.
cat "$PORT" | sed -l -E \
      -e 's/("wifiPassword"[[:space:]]*:[[:space:]]*)"[^"]*"/\1"[REDACTED]"/g' \
      -e 's/("pass"[[:space:]]*:[[:space:]]*)"[^"]*"/\1"[REDACTED]"/g' \
      -e 's/(wifiPassword=)[^&"[:space:]]*/\1[REDACTED]/g' \
  | while IFS= read -r line; do
      printf '%s %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$line"
    done >> "$LOG"
