#!/usr/bin/env bash
# After a VM rollout, run on the Mac: remove what setup.sh created.
#
# The VM deletes its own service account when the last version passes. The bindings that
# named it then grant nothing, but they stay in the policies as "deleted:serviceAccount:...";
# this removes them, deletes the account if it still exists, and turns the VM's timer off.
set -uo pipefail
PROJECT="solar-tracker-442903"
SA="ota-rollout@$PROJECT.iam.gserviceaccount.com"
TOKEN="$(cat "$(cd "$(dirname "$0")/.." && pwd)/.ota_token")"

ssh -o BatchMode=yes vm 'systemctl --user disable --now ota-rollout.timer; rm -f ~/.config/ota-rollout/sa-key.json' < /dev/null

if gcloud iam service-accounts describe "$SA" --project "$PROJECT" > /dev/null 2>&1 < /dev/null; then
  gcloud iam service-accounts delete "$SA" --project "$PROJECT" --quiet < /dev/null && echo "deleted $SA"
else
  echo "$SA already gone"
fi
gcloud iam roles describe otaRolloutSelfDelete --project "$PROJECT" > /dev/null 2>&1 < /dev/null \
  && gcloud iam roles delete otaRolloutSelfDelete --project "$PROJECT" --quiet < /dev/null > /dev/null \
  && echo "deleted custom role otaRolloutSelfDelete"

# Whatever form the member now takes (live, or deleted:...?uid=...), drop every binding for it.
python3 - "$PROJECT" "$SA" "$TOKEN" <<'PY'
import json, subprocess, sys
project, sa, token = sys.argv[1:]
def run(*a, stdin=None):
    return subprocess.run(a, input=stdin, capture_output=True, text=True, stdin=None if stdin else subprocess.DEVNULL)
targets = [
    (["gcloud", "projects", "get-iam-policy", project, "--format=json"],
     ["gcloud", "projects", "set-iam-policy", project, "/dev/stdin", "--format=none"]),
    (["gcloud", "storage", "buckets", "get-iam-policy", "gs://solar-tracker-442903-telemetry", "--format=json"],
     ["gcloud", "storage", "buckets", "set-iam-policy", "gs://solar-tracker-442903-telemetry", "/dev/stdin", "--format=none"]),
    (["gcloud", "storage", "buckets", "get-iam-policy", "gs://solar-tracker-442903-ota", "--format=json"],
     ["gcloud", "storage", "buckets", "set-iam-policy", "gs://solar-tracker-442903-ota", "/dev/stdin", "--format=none"]),
]
for get, put in targets:
    got = run(*get)
    if got.returncode != 0:
        print(f"could not read {get[3]}: {got.stderr.replace(token, '<token>')[:300]}")
        continue
    policy = json.loads(got.stdout)
    removed = 0
    for b in policy.get("bindings", []):
        keep = [m for m in b["members"] if sa not in m]
        removed += len(b["members"]) - len(keep)
        b["members"] = keep
    policy["bindings"] = [b for b in policy.get("bindings", []) if b["members"]]
    if any("condition" in b for b in policy["bindings"]):
        policy["version"] = 3          # a policy with conditional bindings is only accepted as v3
    if removed:
        r = run(*put, stdin=json.dumps(policy))
        print(f"{get[-2] if 'storage' in get else 'project'}: removed {removed} binding(s)"
              + ("" if r.returncode == 0 else f" FAILED: {r.stderr.replace(token, '<token>')[:300]}"))
PY
