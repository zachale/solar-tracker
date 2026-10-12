#!/usr/bin/env bash
# What the field tracker is doing during a staged rollout, from the cloud only. Read-only.
#
#   tools/ota/rollout-status.sh [hours]     default: the last 6 hours
#
# Prints the version the manifest names, every firmware version the tracker has reported in
# its clock sync (/time/est?fw=<version>, from v12.1 on) with when it was first and last
# seen, any sync without a version (v12 or older: a rollback), and a summary of the rows that
# arrived in the window. Row times are site time (EDT).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
HOURS="${1:-6}"
BUCKET="solar-tracker-442903-telemetry"
TOKEN="$(cat "$HERE/.ota_token")"
SINCE="$(date -u -v-"${HOURS}"H +%Y-%m-%dT%H:%M:%SZ)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "rollout status, last $HOURS h (since $SINCE; now $(date -u +%Y-%m-%dT%H:%M:%SZ))"
# Over plain HTTPS, as publish.sh reads it: gcloud storage cat of this one object sometimes
# never returns.
echo "manifest: $(curl -fsS -m 20 "https://storage.googleapis.com/solar-tracker-442903-ota/$TOKEN/manifest.txt" 2>/dev/null || echo "(could not read)")"

gcloud logging read "resource.type=\"cloud_run_revision\" AND resource.labels.service_name=\"test-image\" AND httpRequest.requestUrl:\"/time/est\" AND timestamp>=\"$SINCE\"" \
  --limit=1000 --format="value(timestamp,httpRequest.requestUrl,httpRequest.status)" 2>/dev/null < /dev/null | sort > "$WORK/syncs"
python3 - "$WORK/syncs" <<'PY'
import sys, re
seen = {}
for line in open(sys.argv[1]):
    parts = line.split("\t")
    if len(parts) < 2:
        continue
    t, url = parts[0][:19], parts[1]
    m = re.search(r"[?&]fw=([^&\s]+)", url)
    v = m.group(1) if m else "(none: v12 or older)"
    first, last, n = seen.get(v, (t, t, 0))
    seen[v] = (min(first, t), max(last, t), n + 1)
if not seen:
    print("clock syncs: none in the window")
for v, (first, last, n) in sorted(seen.items(), key=lambda kv: kv[1][0]):
    print(f"clock syncs fw={v}: {n}, first {first}Z, last {last}Z")
PY

for day in $(python3 -c "
from datetime import datetime, timedelta, timezone
s = datetime.strptime('$SINCE', '%Y-%m-%dT%H:%M:%SZ'); e = datetime.now(timezone.utc).replace(tzinfo=None)
d = s.date()
while d <= e.date(): print(d); d += timedelta(days=1)"); do
  gcloud storage ls "gs://$BUCKET/raw/$day/*.ndjson" 2>/dev/null < /dev/null
done | awk -F/ -v since="$(tr ':' '-' <<< "$SINCE")" '$NF >= since' > "$WORK/objects"
# Eight at a time, with curl and one access token: gcloud storage cat sometimes never returns,
# and a hung one held the whole check for over ten minutes.
mkdir -p "$WORK/batches"
ACCESS="$(gcloud auth print-access-token < /dev/null 2>/dev/null)"
export ACCESS
sed 's|^gs://\([^/]*\)/|\1 |' "$WORK/objects" | xargs -P 8 -n 2 sh -c '
  obj="$(python3 -c "import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=\"\"))" "$1")"
  curl -fsS -m 30 --retry 2 -H "Authorization: Bearer $ACCESS" \
    "https://storage.googleapis.com/storage/v1/b/$0/o/$obj?alt=media" -o "'"$WORK"'/batches/$(basename "$1")" 2>/dev/null' 
cat "$WORK/batches"/* > "$WORK/rows" 2>/dev/null || : > "$WORK/rows"
python3 - "$WORK/rows" "$(wc -l < "$WORK/objects" | tr -d ' ')" <<'PY'
import sys, json
from collections import Counter
rows = sorted((json.loads(l) for l in open(sys.argv[1]) if l.strip()), key=lambda r: r["time"])
print(f"batches: {sys.argv[2]}, rows: {len(rows)}")
if not rows:
    sys.exit()
print(f"rows span {rows[0]['time'][:16]} to {rows[-1]['time'][:16]} site time")
print("status:", dict(Counter(r["status"] for r in rows)))
winds = [r["wind_peak"] for r in rows if r["button_panel"]["wind_sensor"]]
frac = [w for w in winds if w != int(w)]
print(f"wind_peak: {len(winds)} readings, max {max(winds) if winds else '-'}, with a fraction: {len(frac)}, e.g. {sorted(set(frac))[:6]}")
temps = [r["temperature"] for r in rows]
print(f"temperature: min {min(temps)}, max {max(temps)}, at or above 128: {sum(t >= 128 for t in temps)}, below 0: {sum(t < 0 for t in temps)}")
night = Counter(r["extended_percent"] for r in rows if r["status"] == "NIGHT")
print("panel % while NIGHT:", dict(sorted(night.items())) if night else "no NIGHT rows")
last = rows[-6:]
print("last rows:", "; ".join(f"{r['time'][11:16]} {r['status']} {r['extended_percent']}% wind {r['wind_peak']} {r['temperature']}C" for r in last))
PY
