#!/usr/bin/env bash
# Overnight escalation ladder. Each phase raises the stress on the I2C bus.
# Every phase bus-clears at boot, auto-recovers on a wedge, and keeps soaking so
# we measure a RATE, not just a first failure. Nothing destructive: no actuator,
# no writes to the RTC, no deletion of anything but bench files.
set -uo pipefail
BENCH="$(cd "$(dirname "$0")" && pwd)"
PORT=/dev/cu.usbmodemC04E3011DB182
FQBN=arduino:renesas_uno:unor4wifi
SUM="$BENCH/logs/ESCALATION.md"

phase () {
  local NAME=$1 SECS=$2 POLL=$3 SPOOL=$4 SYNC=$5 HZ=$6 DESC=$7
  local FLAGS="-DCFG_SD=1 -DCFG_WIFI=1 -DCFG_NAME=\"$NAME\" -DPOLL_MS_V=$POLL -DSPOOL_MS_V=$SPOOL -DSYNC_MS_V=$SYNC -DI2C_HZ=$HZ"
  local LOG="$BENCH/logs/esc-$NAME-$(date +%Y%m%dT%H%M%S).log"
  echo "### phase $NAME ($DESC) ${SECS}s -> $LOG"
  arduino-cli compile --fqbn "$FQBN" --build-property "compiler.cpp.extra_flags=$FLAGS" "$BENCH/12_matrix" >/dev/null 2>&1 || { echo "COMPILE FAIL $NAME"; return 1; }
  pkill -f "cat $PORT" 2>/dev/null; sleep 2
  arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$BENCH/12_matrix" >/dev/null 2>&1 || { echo "UPLOAD FAIL $NAME"; return 1; }
  sleep 4; stty -f "$PORT" 9600 raw -echo
  ( cat "$PORT" | while IFS= read -r l; do printf '%s %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$l"; done >> "$LOG" ) &
  local CP=$! START=$(date +%s)
  while [ $(( $(date +%s) - START )) -lt "$SECS" ]; do sleep 30; done
  kill $CP 2>/dev/null; pkill -P $CP 2>/dev/null; sleep 1

  local n=$(grep -c 'MTX cfg=' "$LOG") b=$(grep -c '<<BAD>>' "$LOG") w=$(grep -c 'MTX WEDGE' "$LOG") r=$(grep -c 'SOFTWARE_RECOVERY_WORKED' "$LOG")
  local mins=$(( ($(date +%s) - START) / 60 ))
  local ttf=$(grep -m1 'MTX WEDGE' "$LOG" 2>/dev/null | sed -E 's/.*minutes=([0-9.]+).*/\1/')
  printf '| %s | %s | %s min | %s | %s | %s | %s | %s |\n' \
    "$NAME" "$DESC" "$mins" "$n" "$b" "$w" "${ttf:--}" "$r" >> "$SUM"
  echo "### $NAME done: polls=$n bad=$b wedges=$w ttf=${ttf:--} recovered=$r"
}

{ echo "# Escalation ladder — overnight $(date '+%Y-%m-%d %H:%M %Z')"
  echo
  echo "All phases: SD + WiFi both on, actuator suppressed, RTC never written."
  echo
  echo "| phase | stress | ran | polls | bad | wedges | first TTF | sw-recovered |"
  echo "|---|---|---|---|---|---|---|---|"; } > "$SUM"

#      name secs   poll  spool   sync    hz      description
phase  E   12600  1000  15000   120000  100000  "aggressive: 1s poll, 15s spool, 2min sync"
phase  F   12600  1000  15000   120000  400000  "aggressive + 400kHz bus"
phase  G   10800   250   5000    60000  400000  "maximum: 250ms poll, 5s spool, 1min sync, 400kHz"

echo "=== ESCALATION COMPLETE ==="; cat "$SUM"
