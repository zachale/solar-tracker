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

# Enough lines for the window even when the log is at its chattiest: 12000 covers a little
# over an hour, and a longer window, such as a whole run, gets 200 for every minute of it.
TAIL="$(python3 -c "
from datetime import datetime, timezone
s = datetime.strptime('$SINCE', '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=timezone.utc)
print(max(12000, int((datetime.now(timezone.utc) - s).total_seconds() / 60) * 200))")"
cat > "$WORK/tail.ps1" <<'PS'
param([int]$Tail)
Get-ChildItem 'C:\Users\zachl\Documents\Arduino\solar-tracker\docs\diagnostics\serial-*.log' |
  Sort-Object LastWriteTime -Descending | Select-Object -First 2 | Sort-Object LastWriteTime |
  ForEach-Object { Get-Content $_.FullName -Tail $Tail }
PS
scp -q "$WORK/tail.ps1" 'home:C:/Users/zachl/ota-flash/monitor-tail.ps1'
ssh -o BatchMode=yes home "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\Users\\zachl\\ota-flash\\monitor-tail.ps1 -Tail $TAIL" \
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
# The newest batch itself, to check the rows as well as their arrival. With an end to the
# window, the newest one inside it: batches are named by the UTC time they landed.
CUT="$(tr ':' '-' <<< "$UNTIL")"
LATEST="$(grep "\.ndjson$" "$WORK/objects" | awk -F/ -v cut="$CUT" 'cut == "" || $NF < cut' | sort | tail -1)"
[[ -n "$LATEST" ]] && gcloud storage cat "$LATEST" > "$WORK/latest.ndjson" 2>/dev/null
touch "$WORK/latest.ndjson"

echo "tracker health since $SINCE (checked $(date -u +%Y-%m-%dT%H:%M:%SZ))"
LATEST_BATCH="$WORK/latest.ndjson" python3 "$HERE/analyze.py" "$SINCE" "$WORK/log" "$WORK/objects" "$HERE/schedule.json" ${UNTIL:+"$UNTIL"}
