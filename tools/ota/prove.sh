#!/usr/bin/env bash
# Prove an over-the-air update end to end on the tracker at the home PC, and keep the
# evidence.
#
#   tools/ota/prove.sh <base> <next> [<next> ...]      e.g.  prove.sh v7 v8 v9
#
# 1. Every <next> is flashed over USB first, with no manifest published, and must boot and
#    report its own version. Nothing that has not been seen to boot is ever published.
# 2. <base> is published and flashed over USB, and must report "up to date".
# 3. Each <next> in turn is published, and the board must pick it up by itself: download in
#    the background while its loop keeps running, verify, flash itself, come back up as
#    <next>, and report "up to date" again.
#
# Every step waits on a line the board prints, only counting lines logged after the step
# began. The serial lines for the whole run are saved to tools/ota/proofs/.
# All versions must already be built with build.sh.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUCKET="solar-tracker-442903-ota"
TOKEN="$(cat "$HERE/.ota_token")"
BASE_VERSION="${1:?usage: $0 <base> <next> [<next> ...]}"
shift
NEXT=("$@")
[[ ${#NEXT[@]} -gt 0 ]] || { echo "usage: $0 <base> <next> [<next> ...]" >&2; exit 1; }

PROOF_DIR="$HERE/proofs"
mkdir -p "$PROOF_DIR"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
PROOF="$PROOF_DIR/$STAMP-$BASE_VERSION-to-${NEXT[${#NEXT[@]}-1]}.log"
RUN_START="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

say() { printf '%s  %s\n' "$(date -u +%H:%M:%SZ)" "$*"; }
die() { say "FAIL: $*"; save_proof; exit 1; }

# Serial lines logged at or after $1. The capture starts a new file at local midnight, so
# the two newest files are read in order and a run that crosses midnight keeps its lines.
cat > /tmp/ota-log-tail.ps1 <<'PS'
Get-ChildItem 'C:\Users\zachl\Documents\Arduino\solar-tracker\docs\diagnostics\serial-*.log' |
  Sort-Object LastWriteTime -Descending | Select-Object -First 2 | Sort-Object LastWriteTime |
  ForEach-Object { Get-Content $_.FullName -Tail 3000 }
PS
scp -q /tmp/ota-log-tail.ps1 'home:C:/Users/zachl/ota-flash/log-tail.ps1'

lines_since() {
  ssh -o BatchMode=yes home 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\zachl\ota-flash\log-tail.ps1' \
    2>/dev/null | tr -d '\r' | awk -v t="$1" '$1 >= t'
}

# Wait up to $2 seconds for a line matching $1 logged after $3. Fails fast on an OTA failure.
wait_for() {
  local pattern="$1" timeout="$2" since="$3" deadline=$((SECONDS + $2)) out
  while (( SECONDS < deadline )); do
    out="$(lines_since "$since")"
    if grep -qE "$pattern" <<<"$out"; then return 0; fi
    if grep -qE "OTA: .* failed \(" <<<"$out"; then
      grep -E "OTA: .* failed \(" <<<"$out" | tail -1
      return 2
    fi
    sleep 5
  done
  return 1
}

now() { date -u +%Y-%m-%dT%H:%M:%SZ; }

# A script file rather than an inline -Command: the quotes would not survive
# bash -> ssh -> cmd -> PowerShell intact.
cat > /tmp/ota-board-present.ps1 <<'PS'
$b = Get-CimInstance Win32_PnPEntity -Filter "Name LIKE '%(COM%'" |
  Where-Object { $_.PNPDeviceID -match 'VID_2341&PID_1002' -and $_.Status -eq 'OK' }
if ($b) { 'yes' }
PS
scp -q /tmp/ota-board-present.ps1 'home:C:/Users/zachl/ota-flash/board-present.ps1'

board_present() {
  ssh -o BatchMode=yes home 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\zachl\ota-flash\board-present.ps1' 2>/dev/null | grep -q yes
}

# flash-home.sh, failing the run on anything but a clean upload with a capture restarted.
# Not `flash-home | grep FAIL && die`: under pipefail a failed flash makes that pipeline
# non-zero even when grep matches, and the die never runs.
flash() {
  local out
  out="$("$HERE/flash-home.sh" "$1" 2>&1)"
  if [[ $? -ne 0 ]] || grep -q "FAIL" <<<"$out"; then
    printf '%s\n' "$out" | grep -E "FAIL|rror" | head -3
    die "USB flash of $1 failed"
  fi
}

set_manifest() {
  printf '%s\n' "$1" | gcloud storage cp - "gs://$BUCKET/$TOKEN/manifest.txt" \
    --cache-control=no-store --content-type=text/plain --quiet >/dev/null 2>&1
}

save_proof() {
  lines_since "$RUN_START" | grep -vE "^[^ ]+ (Wind Speed|PercentExtended|Settings Server|[0-9]+:[0-9]+:[0-9]+$|[0-9]{10}$)" > "$PROOF"
  say "evidence saved to ${PROOF#$HERE/}"
}

say "waiting for the board on USB"
until board_present; do sleep 10; done
sleep 10
board_present || die "board dropped off USB again"
say "board present"

for v in "${NEXT[@]}"; do
  say "pre-flight: $v must boot over USB with no manifest published"
  gcloud storage rm "gs://$BUCKET/$TOKEN/manifest.txt" --quiet >/dev/null 2>&1
  t="$(now)"
  flash "$v"
  wait_for "Firmware version: $v" 90 "$t" || die "$v did not boot"
  wait_for "OTA: no usable manifest, staying on $v" 90 "$t" || die "$v booted but never checked the manifest"
  say "  $v boots and checks in"
done

say "base: publishing $BASE_VERSION and flashing it over USB"
"$HERE/publish.sh" "$BASE_VERSION" >/dev/null || die "publish $BASE_VERSION"
t="$(now)"
flash "$BASE_VERSION"
wait_for "Firmware version: $BASE_VERSION" 90 "$t" || die "$BASE_VERSION did not boot"
wait_for "OTA: up to date at $BASE_VERSION" 90 "$t" || die "$BASE_VERSION did not report up to date"
say "  running $BASE_VERSION, manifest agrees"

for v in "${NEXT[@]}"; do
  say "over the air: publishing $v, nothing else touches the board"
  t="$(now)"
  "$HERE/publish.sh" "$v" >/dev/null || die "publish $v"
  wait_for "OTA: downloading $v" 420 "$t"; r=$?
  (( r == 0 )) || die "the board never started downloading $v"
  say "  downloading in the background"
  wait_for "OTA: image verified, flashing $v" 300 "$t" || die "$v never verified"
  say "  verified, the board is flashing itself"
  wait_for "Firmware version: $v" 180 "$t" || die "the board did not come back up as $v"
  wait_for "OTA: up to date at $v" 120 "$t" || die "$v came up but did not confirm the manifest"
  say "  running $v, installed over the air"
done

save_proof
say "PASS: $BASE_VERSION -> ${NEXT[*]}, every hop over the air"
