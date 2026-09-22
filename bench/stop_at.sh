#!/usr/bin/env bash
# Stop the matrix orchestrator cleanly at a given local time, recording the
# in-flight config as PARTIAL with its real elapsed time and confidence.
set -uo pipefail
BENCH="$(cd "$(dirname "$0")" && pwd)"
TARGET="$1"                       # e.g. "13:45"
SUMMARY="$BENCH/logs/MATRIX_RESULTS.md"

now=$(date +%s)
stop=$(date -j -f "%Y-%m-%d %H:%M:%S" "$(date +%Y-%m-%d) $TARGET:00" +%s 2>/dev/null)
[ "$stop" -le "$now" ] && stop=$((stop + 86400))
wait=$((stop - now))
echo "stopping at $TARGET local ($(date -r $stop)) — sleeping ${wait}s"
sleep "$wait"

# identify the config still running
L=$(ls -t "$BENCH"/logs/matrix-*.log 2>/dev/null | head -1)
CFG=$(basename "$L" | sed -E 's/matrix-([A-D])-.*/\1/')

pkill -f matrix_run.sh 2>/dev/null
sleep 1
pkill -f "cat /dev/cu.usbmodem" 2>/dev/null
sleep 2

n=$(grep -c 'MTX cfg=' "$L"); b=$(grep -c '<<BAD>>' "$L"); w=$(grep -c 'MTX WEDGE' "$L")
s=$(head -1 "$L" | cut -d' ' -f1); e=$(tail -1 "$L" | cut -d' ' -f1)
mins=$(python3 -c "
from datetime import datetime
a=datetime.strptime('$s','%Y-%m-%dT%H:%M:%SZ');b=datetime.strptime('$e','%Y-%m-%dT%H:%M:%SZ')
print(int((b-a).total_seconds()//60))")
conf=$(python3 -c "import math;print(f'{(1-math.exp(-$mins/150))*100:.1f}')")
res="PARTIAL-CLEAN"; [ "$w" -gt 0 ] && res="WEDGED"

printf '| %s | SD=1 WIFI=1 | %s | %s | %s min | %s%% |\n' \
  "$CFG" "$res" "$( [ "$w" -gt 0 ] && grep -m1 'MTX WEDGE' "$L" | sed -E 's/.*minutes=([0-9.]+).*/\1/' || echo '-' )" \
  "$mins" "$conf" >> "$SUMMARY"

{
  echo
  echo "Stopped cleanly at $(date -u '+%Y-%m-%dT%H:%M:%SZ') for a MacBook unplug."
  echo "Config $CFG accumulated $mins min of exposure ($conf% confidence)."
  echo "Exposure is additive for a memoryless process: resume $CFG later and add the time."
  echo "Needs $(python3 -c "print(max(0,450-$mins))") more minutes to reach 95%."
} >> "$SUMMARY"

echo "=== stopped. $CFG: polls=$n bad=$b wedges=$w ran=${mins}min conf=${conf}% ==="
cat "$SUMMARY"
