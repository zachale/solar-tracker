#!/usr/bin/env bash
# The hourly health check of the bench tracker: is it alive, is its clock right, did it heal
# any I2C wedges, did it command the scheduled extension, did its batches reach the cloud,
# and what is its updater doing. Read-only: nothing here writes to the board or the bucket.
#
#   tools/monitor/hourly.sh [since ISO [until ISO]]    default: the last 65 minutes
#
# The serial log comes from the capture on the home PC (ssh host "home"), the batches from
# the telemetry bucket. The schedule the board runs is kept in schedule.json, refreshed
# whenever a boot's "Config doc:" line is in the window.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUCKET="solar-tracker-442903-telemetry"
SINCE="${1:-$(date -u -v-65M +%Y-%m-%dT%H:%M:%SZ)}"
UNTIL="${2:-}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Enough lines for a little over an hour even when the log is at its chattiest.
cat > "$WORK/tail.ps1" <<'PS'
Get-ChildItem 'C:\Users\zachl\Documents\Arduino\solar-tracker\docs\diagnostics\serial-*.log' |
  Sort-Object LastWriteTime -Descending | Select-Object -First 2 | Sort-Object LastWriteTime |
  ForEach-Object { Get-Content $_.FullName -Tail 12000 }
PS
scp -q "$WORK/tail.ps1" 'home:C:/Users/zachl/ota-flash/monitor-tail.ps1'
ssh -o BatchMode=yes home 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\zachl\ota-flash\monitor-tail.ps1' \
  2>/dev/null | tr -d '\r' > "$WORK/log"

CONFIG="$(grep -A1 "Config doc:" "$WORK/log" | tail -1 | cut -d' ' -f2-)"
if [[ "$CONFIG" == \{* ]]; then
  python3 - "$CONFIG" "$HERE/schedule.json" <<'PY'
import json, sys
schedule = json.loads(json.loads(sys.argv[1])["schedule"])
json.dump({k: float(v) for k, v in schedule.items()}, open(sys.argv[2], "w"))
PY
fi

for day in $(python3 -c "
from datetime import datetime, timedelta, timezone
s = datetime.strptime('$SINCE', '%Y-%m-%dT%H:%M:%SZ')
e = datetime.now(timezone.utc).replace(tzinfo=None)
d = s.date()
while d <= e.date():
    print(d); d += timedelta(days=1)"); do
  gcloud storage ls "gs://$BUCKET/raw/$day/" 2>/dev/null
done > "$WORK/objects"

echo "tracker health since $SINCE (checked $(date -u +%Y-%m-%dT%H:%M:%SZ))"
python3 "$HERE/analyze.py" "$SINCE" "$WORK/log" "$WORK/objects" "$HERE/schedule.json" ${UNTIL:+"$UNTIL"}
