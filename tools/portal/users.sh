#!/usr/bin/env bash
# Who may sign in to the tracker page, and their personal links: users.json in the portal bucket,
# which the portal API reads.
#
#   tools/portal/users.sh list
#   tools/portal/users.sh add <id> <name>      new person; prints their link
#   tools/portal/users.sh link <id>            re-issue their link; the old one stops working
#   tools/portal/users.sh remove <id>
#
# <id> is 1 to 32 of a-z, 0-9 and -. A link is ${PORTAL_URL:-https://zachlegesse.ca/tracker}#key=<key>
# with a fresh 32-byte random key, and opening it signs in that device as that person, for good.
# Only the key's SHA-256 goes into users.json; the key itself exists only in the link printed
# here, so it is never written to a file, and a lost link cannot be printed again, only re-issued.
# The portal re-reads users.json every 30 s, so a removal or a re-issue signs out every device
# concerned within half a minute. users.json is created by the first add, and entries it does not
# recognize (such as the old phone-number format) are dropped by the next change.
set -euo pipefail
BUCKET="${PORTAL_BUCKET:-solar-tracker-442903-portal}"
OBJECT="gs://$BUCKET/users.json"
URL="${PORTAL_URL:-https://zachlegesse.ca/tracker}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

usage() {
  echo "usage: $0 list | add <id> <name> | link <id> | remove <id>" >&2
  exit 2
}
ACTION="${1:-}"
case "$ACTION" in
  list) [[ $# -eq 1 ]] || usage ;;
  add) [[ $# -eq 3 ]] || usage ;;
  link | remove) [[ $# -eq 2 ]] || usage ;;
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

# Writes the new list to new.json and the message (with the link, for add and link) to stdout.
edit() {
  python3 - "$@" <<'PY'
import hashlib, json, re, secrets, sys
from datetime import datetime, timezone

src, dst, url, action, *args = sys.argv[1:]

def usable(u):
    # As portal/src/users.js: anything else is ignored by the portal.
    return (isinstance(u, dict)
            and isinstance(u.get("id"), str) and re.fullmatch(r"[a-z0-9-]{1,32}", u["id"])
            and isinstance(u.get("keyHash"), str) and re.fullmatch(r"[0-9a-f]{64}", u["keyHash"])
            and isinstance(u.get("name"), str) and u["name"].strip())

try:
    loaded = json.load(open(src))
except ValueError as e:
    sys.exit(f"users.json is not JSON ({e}); fix or remove it by hand")
entries = loaded.get("users") if isinstance(loaded, dict) else None
entries = entries if isinstance(entries, list) else []
users = [u for u in entries if usable(u)]
if len(users) != len(entries):
    print(f"note: ignoring {len(entries) - len(users)} entries in users.json that are not in the current"
          f" format; the portal ignores them too{'' if action == 'list' else ', and this change drops them'}",
          file=sys.stderr)

if action == "list":
    if not users:
        print("no users")
    for u in users:
        issued = str(u.get("issuedAt", ""))
        try:
            issued = datetime.fromisoformat(issued.replace("Z", "+00:00")).astimezone().strftime("%Y-%m-%d")
        except ValueError:
            pass
        print(f"{u['id']:<16} {u['name']:<24} issued {issued}")
    sys.exit()

uid = args[0]
if not re.fullmatch(r"[a-z0-9-]{1,32}", uid):
    sys.exit(f"not an id: {uid!r} (1 to 32 of a-z, 0-9 and -)")
existing = next((u for u in users if u["id"] == uid), None)

def issue(u):
    # 32 random bytes, base64url: 43 characters. Only its hash is kept.
    key = secrets.token_urlsafe(32)
    u["keyHash"] = hashlib.sha256(key.encode()).hexdigest()
    u["issuedAt"] = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    return f"{url}#key={key}"

if action == "add":
    if existing:
        sys.exit(f"{uid} is already in users.json; use 'link {uid}' to give them a new link")
    name = args[1].strip()
    if not name:
        sys.exit("the name is empty")
    person = {"id": uid, "name": name}
    link = issue(person)
    users.append(person)
    print(f"added {uid} ({name}). Their link:\n\n{link}\n")
    print(f"It signs in any device that opens it as {name}, so send it only to them.")
elif existing is None:
    sys.exit(f"{uid} is not in users.json")
elif action == "link":
    link = issue(existing)
    print(f"new link for {uid} ({existing['name']}):\n\n{link}\n")
    print("The old link, and every device signed in with it, stop working within 30 s.")
    print(f"This one signs in any device that opens it as {existing['name']}, so send it only to them.")
else:
    users.remove(existing)
    print(f"removed {uid} ({existing['name']}); every device signed in as them is signed out within 30 s")

with open(dst, "w") as f:
    json.dump({"users": users}, f, indent=2)
    f.write("\n")
PY
}

# The message is held in a shell variable, not a file, and printed only once the upload has
# worked: a link whose hash never reached the bucket would not sign anyone in.
MESSAGE="$(edit "$WORK/users.json" "$WORK/new.json" "$URL" "$@")"

if [[ "$ACTION" != "list" ]]; then
  gcloud storage cp "$WORK/new.json" "$OBJECT" --content-type=application/json --quiet < /dev/null
fi
printf '%s\n' "$MESSAGE"
