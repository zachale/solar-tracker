#!/usr/bin/env bash
# Run the A/B/C/D matrix. Each config soaks until it wedges or until the
# confidence deadline, whichever comes first, then moves on.
#
#   95% confidence that a config is NOT broken, at an observed MTTF of 150 min:
#   t = 150 * ln(20) = 449 min = 7.5 h
set -uo pipefail
BENCH="$(cd "$(dirname "$0")" && pwd)"
PORT=/dev/cu.usbmodemC04E3011DB182
FQBN=arduino:renesas_uno:unor4wifi
DEADLINE=${DEADLINE:-27000}          # 7.5 h in seconds
SUMMARY="$BENCH/logs/MATRIX_RESULTS.md"

run_cfg () {
  local NAME=$1 SD=$2 WIFI=$3
  local FLAGS="-DCFG_SD=$SD -DCFG_WIFI=$WIFI -DCFG_NAME=\"$NAME\""
  local LOG="$BENCH/logs/matrix-$NAME-$(date +%Y%m%dT%H%M%S).log"

  echo "### config $NAME  SD=$SD WIFI=$WIFI  -> $LOG"
  arduino-cli compile --fqbn "$FQBN" --build-property "compiler.cpp.extra_flags=$FLAGS" "$BENCH/12_matrix" >/dev/null 2>&1 \
    || { echo "COMPILE FAIL $NAME"; return 1; }
  pkill -f "cat $PORT" 2>/dev/null; sleep 2
  arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$BENCH/12_matrix" >/dev/null 2>&1 \
    || { echo "UPLOAD FAIL $NAME"; return 1; }
  sleep 4
  stty -f "$PORT" 9600 raw -echo

  ( cat "$PORT" | while IFS= read -r l; do printf '%s %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$l"; done >> "$LOG" ) &
  local CP=$!
  local START=$(date +%s)
  local RESULT="CLEAN" TTF=""

  while true; do
    sleep 20
    if grep -q "MTX WEDGE" "$LOG" 2>/dev/null; then
      TTF=$(grep -m1 "MTX WEDGE" "$LOG" | sed -E 's/.*minutes=([0-9.]+).*/\1/')
      RESULT="WEDGED"
      # let it log the wedged state for 10 more minutes, then move on
      sleep 600
      break
    fi
    [ $(( $(date +%s) - START )) -ge "$DEADLINE" ] && break
  done

  kill $CP 2>/dev/null; pkill -P $CP 2>/dev/null; sleep 1
  local ELAPSED=$(( ($(date +%s) - START) / 60 ))
  local CONF=$(python3 -c "import math;print(f'{(1-math.exp(-$ELAPSED/150))*100:.1f}')")
  printf '| %s | SD=%s WIFI=%s | %s | %s | %s min | %s%% |\n' \
    "$NAME" "$SD" "$WIFI" "$RESULT" "${TTF:--}" "$ELAPSED" "$CONF" >> "$SUMMARY"
  echo "### $NAME done: $RESULT ttf=${TTF:--} elapsed=${ELAPSED}min"
}

{
  echo "# Matrix results"
  echo
  echo "Deadline per config: $((DEADLINE/60)) min. MTTF baseline 150 min."
  echo
  echo "| cfg | build | result | TTF | ran | conf. not-broken |"
  echo "|---|---|---|---|---|---|"
} > "$SUMMARY"

run_cfg C 0 0      # baseline: RTC only
run_cfg B 0 1      # WiFi only
run_cfg A 1 0      # SD only
run_cfg D 1 1      # positive control: both

echo "=== MATRIX COMPLETE ==="
cat "$SUMMARY"
