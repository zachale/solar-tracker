#!/usr/bin/env bash
# Sign-in requests for the tracker page: list them, approve one, deny one.
#
#   tools/portal/requests.sh list
#   tools/portal/requests.sh approve <id>
#   tools/portal/requests.sh deny <id>
#
# Someone on the allowlist (users.sh) asks to sign in on the page; the portal API files the
# request under requests/ in the portal bucket and the page waits. Approving it here signs in
# that browser, and only that one, the next time it checks. This does what the admin view on the
# page does, and is how the first admin gets in before there is an admin view to use. <id> can
# be shortened to any unique prefix of 6 or more characters. Requests last 24 hours; list shows
# the ones still open, including answered ones the browser has not collected yet.
set -euo pipefail
BUCKET="${PORTAL_BUCKET:-solar-tracker-442903-portal}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

usage() {
  echo "usage: $0 list | approve <id> | deny <id>" >&2
  exit 2
}
ACTION="${1:-}"
case "$ACTION" in
  list) [[ $# -eq 1 ]] || usage ;;
  approve | deny) [[ $# -eq 2 ]] || usage ;;
  *) usage ;;
esac

# Each request object is one line of JSON, so reading them all at once gives NDJSON.
if ! gcloud storage cat "gs://$BUCKET/requests/*.json" > "$WORK/requests" 2> "$WORK/err" < /dev/null; then
  grep -q "matched no objects" "$WORK/err" || { cat "$WORK/err" >&2; exit 1; }
fi

mkdir "$WORK/decided"
python3 - "$WORK/requests" "$WORK/decided" "$@" <<'PY'
import json, os, sys
from datetime import datetime, timezone

src, out, action, *args = sys.argv[1:]
now = datetime.now(timezone.utc)
when = lambda iso: datetime.fromisoformat(iso.replace("Z", "+00:00"))
live = [r for r in (json.loads(line) for line in open(src) if line.strip()) if when(r["expiresAt"]) > now]

def describe(r):
    asked = when(r["requestedAt"]).astimezone()
    left = (when(r["expiresAt"]) - now).total_seconds() / 3600
    return f"{r['name']} ({r['phone']}), asked {asked:%a %H:%M}, {left:.0f} h left"

if action == "list":
    if not live:
        print("no open requests")
    for r in sorted(live, key=lambda r: r["requestedAt"], reverse=True):
        print(f"{r['id']}  {r['status']:<8}  {describe(r)}")
    sys.exit()

prefix = args[0].lower()
if len(prefix) < 6:
    sys.exit("give at least 6 characters of the id")
matches = [r for r in live if r["id"].startswith(prefix)]
if len(matches) != 1:
    sys.exit(f"{'more than one open request matches' if matches else 'no open request matches'} {args[0]!r}")

r = matches[0]
r["status"] = "approved" if action == "approve" else "denied"
r["decidedAt"] = now.isoformat(timespec="milliseconds").replace("+00:00", "Z")
# One line, as the portal writes it, so the cat above keeps working.
with open(os.path.join(out, f"{r['id']}.json"), "w") as f:
    f.write(json.dumps(r, separators=(",", ":")) + "\n")
print(f"{r['status']}: {describe(r)}")
PY

[[ "$ACTION" == "list" ]] && exit 0
gcloud storage cp "$WORK"/decided/*.json "gs://$BUCKET/requests/" --content-type=application/json --quiet < /dev/null
