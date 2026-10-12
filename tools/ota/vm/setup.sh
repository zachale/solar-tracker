#!/usr/bin/env bash
# One-time setup, run on the Mac: hand the rest of a staged rollout to the VM.
#
#   tools/ota/vm/setup.sh <current version> <published at, UTC ISO> <next version>...
#   tools/ota/vm/setup.sh v12.2 2026-10-11T18:44:37Z v12.3 v12.4 v12.5
#
# Creates the service account the VM runs as, with only what a rollout needs:
#   - read the project's logs (the tracker's clock syncs name its firmware)
#   - read the telemetry bucket
#   - write one object, the OTA manifest, and nothing else in the OTA bucket
#   - delete itself, which it does when the last version passes (a custom role holding only
#     iam.serviceAccounts.delete, bound on the account itself)
# Its key goes straight to the VM (~/.config/ota-rollout/sa-key.json, mode 600) and the
# local copy is removed. The next versions' images must already be in the bucket
# (publish.sh --no-manifest); the VM only moves the manifest.
#
# The Gmail app password is not handled here: put it on the VM yourself, see the end.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OTA_DIR="$(cd "$HERE/.." && pwd)"
PROJECT="solar-tracker-442903"
OTA_BUCKET="solar-tracker-442903-ota"
TELEMETRY_BUCKET="solar-tracker-442903-telemetry"
SA_NAME="ota-rollout"
SA="$SA_NAME@$PROJECT.iam.gserviceaccount.com"
VM="vm"
EMAIL="zachlegesse@gmail.com"

CURRENT="$1"; PUBLISHED_AT="$2"; shift 2
NEXT=("$@")
TOKEN="$(cat "$OTA_DIR/.ota_token")"
BASE="https://storage.googleapis.com/$OTA_BUCKET/$TOKEN"
quiet() { local out; if ! out="$("$@" 2>&1 < /dev/null)"; then printf '%s\n' "${out//$TOKEN/<token>}" >&2; return 1; fi; }

# Every next image is in the bucket, byte for byte as built.
for v in "${NEXT[@]}"; do
  curl -fsS -m 30 "$BASE/$v.ota" | cmp -s - "$OTA_DIR/out/$v/$v.ota" \
    || { echo "FAIL: $v.ota in the bucket is missing or differs from the build; run publish.sh --no-manifest $v" >&2; exit 1; }
done
[[ "$(curl -fsS -m 20 "$BASE/manifest.txt" | cut -d' ' -f1)" == "$CURRENT" ]] \
  || { echo "FAIL: the manifest does not name $CURRENT" >&2; exit 1; }

echo "service account and its permissions"
ssh -o BatchMode=yes "$VM" 'loginctl show-user "$USER" -p Linger' < /dev/null | grep -qx 'Linger=yes' \
  || { echo "FAIL: lingering is off for the VM user, so its timer would stop with the SSH session (sudo loginctl enable-linger)" >&2; exit 1; }
ssh -o BatchMode=yes "$VM" 'test ! -e ~/.local/state/ota-rollout/state.json' < /dev/null \
  || { echo "FAIL: the VM already has a rollout state; look at it first (rollout_vm.py status), remove it to start over" >&2; exit 1; }

if ! gcloud iam service-accounts describe "$SA" --project "$PROJECT" > /dev/null 2>&1 < /dev/null; then
  quiet gcloud iam service-accounts create "$SA_NAME" --project "$PROJECT" \
    --display-name "Staged OTA rollout (VM)" --description "Deletes itself when the rollout finishes"
  # A new account takes a little while to be usable in IAM policies.
  for _ in $(seq 20); do
    gcloud iam service-accounts describe "$SA" --project "$PROJECT" > /dev/null 2>&1 < /dev/null && break
    sleep 3
  done
  sleep 10
fi
gcloud iam roles describe otaRolloutSelfDelete --project "$PROJECT" > /dev/null 2>&1 < /dev/null \
  || quiet gcloud iam roles create otaRolloutSelfDelete --project "$PROJECT" \
       --title "OTA rollout: delete itself" --permissions iam.serviceAccounts.delete --stage GA
retry() { for i in 1 2 3 4 5; do quiet "$@" && return 0; sleep $((i * 5)); done; return 1; }
retry gcloud projects add-iam-policy-binding "$PROJECT" --member "serviceAccount:$SA" \
  --role roles/logging.viewer --condition None
retry gcloud storage buckets add-iam-policy-binding "gs://$TELEMETRY_BUCKET" --member "serviceAccount:$SA" \
  --role roles/storage.objectViewer
retry gcloud storage buckets add-iam-policy-binding "gs://$OTA_BUCKET" --member "serviceAccount:$SA" \
  --role roles/storage.objectUser \
  --condition "title=manifest only,expression=resource.name == \"projects/_/buckets/$OTA_BUCKET/objects/$TOKEN/manifest.txt\""
retry gcloud iam service-accounts add-iam-policy-binding "$SA" --project "$PROJECT" \
  --member "serviceAccount:$SA" --role "projects/$PROJECT/roles/otaRolloutSelfDelete"

echo "config and state for the VM"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
# HISTORY: optional JSON list of earlier stages, for the summary email at the end.
python3 - "$WORK" "$CURRENT" "$PUBLISHED_AT" "$OTA_DIR" "$EMAIL" "${HISTORY:-[]}" "${NEXT[@]}" <<'PY'
import json, sys, zlib, os
work, current, published_at, ota_dir, email, history, *nxt = sys.argv[1:]
def stage(v):
    d = open(f"{ota_dir}/out/{v}/solar-tracker.ino.bin", "rb").read()
    return {"version": v, "manifest": f"{v} {len(d)} {zlib.crc32(d):08x}",
            "ota_bytes": os.path.getsize(f"{ota_dir}/out/{v}/{v}.ota")}
json.dump({"project": "solar-tracker-442903", "service": "test-image",
           "ota_bucket": "solar-tracker-442903-ota", "telemetry_bucket": "solar-tracker-442903-telemetry",
           "email_from": email, "email_to": email,
           "stages": [stage(v) for v in [current, *nxt]]}, open(f"{work}/config.json", "w"), indent=2)
json.dump({"current": {"version": current, "published_at": published_at},
           "history": json.loads(history) + [{"version": current, "published_at": published_at}]},
          open(f"{work}/state.json", "w"), indent=2)
PY

KEY="$WORK/sa-key.json"
quiet gcloud iam service-accounts keys create "$KEY" --iam-account "$SA" --project "$PROJECT"
printf '%s' "$TOKEN" > "$WORK/ota-token"

echo "copying to the VM"
ssh -o BatchMode=yes "$VM" 'mkdir -p ~/ota-rollout ~/.config/ota-rollout ~/.local/state/ota-rollout ~/.config/systemd/user && chmod 700 ~/.config/ota-rollout' < /dev/null
scp -q "$HERE/rollout_vm.py" "$VM:ota-rollout/"
scp -q "$HERE/ota-rollout.service" "$HERE/ota-rollout.timer" "$VM:.config/systemd/user/"
scp -q "$KEY" "$WORK/ota-token" "$WORK/config.json" "$VM:.config/ota-rollout/"
ssh -o BatchMode=yes "$VM" 'chmod 600 ~/.config/ota-rollout/*' < /dev/null
scp -q "$WORK/state.json" "$VM:.local/state/ota-rollout/state.json"
rm -f "$KEY"

echo "first check, as a dry run"
ssh -o BatchMode=yes "$VM" 'cd ~/ota-rollout && ~/.local/bin/uv run --quiet rollout_vm.py check --dry-run' < /dev/null \
  | sed "s/$TOKEN/<token>/g"

cat <<EOF

Set up. Two steps remain:
  1. Put a Gmail app password for $EMAIL on the VM (https://myaccount.google.com/apppasswords):
       ssh vm 'umask 077; cat > ~/.config/ota-rollout/gmail-app-password'   (paste, Enter, Ctrl-D)
       ssh vm 'cd ~/ota-rollout && ~/.local/bin/uv run rollout_vm.py test-email'
     The check refuses to run without it, since a stop could not be reported.
  2. Start the timer:  ssh vm 'systemctl --user daemon-reload && systemctl --user enable --now ota-rollout.timer'
EOF
