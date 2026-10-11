#!/usr/bin/env bash
# Who may sign in to the tracker page: the allowlist the portal API reads, users.json in the
# portal bucket.
#
#   tools/portal/users.sh list
#   tools/portal/users.sh add <phone> <name> [--admin]
#   tools/portal/users.sh remove <phone>
#
# Numbers are normalized to E.164 by the same rules as portal/src/phone.js, so 519-555-1234
# and +15195551234 are the same person. Adding a number that is already there replaces its
# name and admin flag. users.json is created by the first add. The portal re-reads it every
# 30 s, so someone removed here is signed out within half a minute.
set -euo pipefail
BUCKET="${PORTAL_BUCKET:-solar-tracker-442903-portal}"
OBJECT="gs://$BUCKET/users.json"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

usage() {
  echo "usage: $0 list | add <phone> <name> [--admin] | remove <phone>" >&2
  exit 2
}
ACTION="${1:-}"
case "$ACTION" in
  list) [[ $# -eq 1 ]] || usage ;;
  add) [[ $# -eq 3 || ( $# -eq 4 && "$4" == "--admin" ) ]] || usage ;;
  remove) [[ $# -eq 2 ]] || usage ;;
  *) usage ;;
esac

# A missing file is an empty list. Any other failure (logged out, no access) stops here, so
# that "add" can never replace the real list with a one-person one.
if ! gcloud storage cat "$OBJECT" > "$WORK/users.json" 2> "$WORK/err" < /dev/null; then
  if grep -q "matched no objects" "$WORK/err"; then
    echo '{"users": []}' > "$WORK/users.json"
  else
    cat "$WORK/err" >&2
    exit 1
  fi
fi

python3 - "$WORK/users.json" "$WORK/new.json" "$@" <<'PY'
import json, re, sys

src, dst, action, *args = sys.argv[1:]

def normalize(raw):
    # As portal/src/phone.js.
    trimmed = raw.strip()
    digits = re.sub(r"\D", "", trimmed)
    if trimmed.startswith("+"):
        return "+" + digits if 8 <= len(digits) <= 15 else None
    if len(digits) == 10:
        return "+1" + digits
    if len(digits) == 11 and digits.startswith("1"):
        return "+" + digits
    return None

users = json.load(open(src)).get("users", [])

if action == "list":
    if not users:
        print("no users")
    for u in users:
        print(f"{u['phone']:<16} {u['name']:<20} {'admin' if u.get('admin') else ''}")
    sys.exit()

phone = normalize(args[0])
if phone is None:
    sys.exit(f"not a phone number: {args[0]!r} (10 digits, 11 starting with 1, or +country code)")

kept = [u for u in users if normalize(u["phone"]) != phone]
if action == "add":
    name = args[1].strip()
    if not name:
        sys.exit("the name is empty")
    admin = len(args) == 3
    kept.append({"phone": phone, "name": name, "admin": admin})
    print(f"{'updated' if len(kept) == len(users) else 'added'} {phone} {name}{' (admin)' if admin else ''}")
elif len(kept) == len(users):
    sys.exit(f"{phone} is not in users.json")
else:
    print(f"removed {phone}")

json.dump({"users": kept}, open(dst, "w"), indent=2)
PY

[[ "$ACTION" == "list" ]] && exit 0
gcloud storage cp "$WORK/new.json" "$OBJECT" --content-type=application/json --quiet < /dev/null
