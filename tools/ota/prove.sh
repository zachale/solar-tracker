#!/usr/bin/env bash
# Prove over-the-air updates end to end with production timing, and keep the evidence.
#
#   tools/ota/prove.sh <base> <next> [<next> ...]      e.g.  prove.sh v10 v11 v12
#
# 1. Every image is uploaded without touching the manifest.
# 2. Every <next> is flashed over USB with no manifest published, and must boot, arm its
#    watchdog, download its own rollback copy, and read that copy back identical to itself.
#    Nothing that has not been seen to do all of that is ever pointed at.
# 3. <base> is flashed over USB with the manifest naming it, saves its rollback copy, and must
#    report "up to date".
# 4. Each <next> in turn is named in the manifest and the board must take it by itself:
#    download in the background, check its rollback copy, install on trial, come back as
#    <next>, survive the trial window, confirm, and save <next> as the new rollback copy.
#
# For the failure cases (a bad image, a crash loop, a hang, an abort) see verify.sh.
set -uo pipefail
source "$(cd "$(dirname "$0")" && pwd)/lib.sh"

BASE_VERSION="${1:?usage: $0 <base> <next> [<next> ...]}"
shift
NEXT=("$@")
[[ ${#NEXT[@]} -gt 0 ]] || { echo "usage: $0 <base> <next> [<next> ...]" >&2; exit 1; }

mkdir -p "$HERE/proofs"
RUN_START="$(now)"
PROOF="$HERE/proofs/early-failure.log"
save_proof() { evidence_since "$RUN_START" > "$PROOF"; say "evidence saved to ${PROOF#$HERE/}"; }
die() { say "FAIL: $*"; save_proof; exit 1; }

for v in "$BASE_VERSION" "${NEXT[@]}"; do
  [[ -f "$HERE/out/$v/TEST-IMAGE" ]] && die "$v is a test image; prove.sh is for production images"
  "$HERE/publish.sh" --no-manifest "$v" >/dev/null || die "upload $v"
done
say "uploaded $BASE_VERSION ${NEXT[*]}"

say "waiting for the board on USB"
until board_present; do sleep 10; done
sleep 10
board_present || die "board dropped off USB again"

# Stamped here, not at launch: the script may have been waiting hours for a replug.
RUN_START="$(now)"
PROOF="$HERE/proofs/$(date -u +%Y%m%dT%H%M%SZ)-$BASE_VERSION-to-${NEXT[${#NEXT[@]}-1]}.log"
say "board present"

for v in "${NEXT[@]}"; do
  say "pre-flight: $v must boot and save a verified copy of itself, with no manifest published"
  clear_manifest || die "could not clear the manifest"
  t="$(now)"
  flash "$v" || die "USB flash of $v failed"
  wait_for "Firmware version: $v" 90 "$t" || die "$v did not boot"
  wait_for "Watchdog armed" 30 "$t" || die "$v did not arm its watchdog"
  wait_for "OTA: rollback copy of $v ready" 300 "$t" || die "$v could not save and read back a copy of itself"
  say "  $v boots, arms its watchdog, and saves a verified copy of itself"
done

say "base: $BASE_VERSION over USB, manifest naming it"
set_manifest "$BASE_VERSION" || die "set manifest $BASE_VERSION"
t="$(now)"
flash "$BASE_VERSION" || die "USB flash of $BASE_VERSION failed"
wait_for "Firmware version: $BASE_VERSION" 90 "$t" || die "$BASE_VERSION did not boot"
wait_for "OTA: rollback copy of $BASE_VERSION ready" 300 "$t" || die "$BASE_VERSION never saved its rollback copy"
wait_for "OTA: up to date at $BASE_VERSION" 420 "$t" || die "$BASE_VERSION did not report up to date"
say "  running $BASE_VERSION with a verified rollback copy"

for v in "${NEXT[@]}"; do
  say "over the air: manifest now names $v; nothing else touches the board"
  t="$(now)"
  set_manifest "$v" || die "set manifest $v"
  wait_for "OTA: downloading $v, [0-9]+ bytes, in the background" 420 "$t" || die "the board never started downloading $v"
  say "  downloading in the background"
  wait_for "OTA: image verified, installing $v" 300 "$t" || die "$v never verified"
  say "  verified; rollback copy checked; installing on trial"
  wait_for "Firmware version: $v" 180 "$t" || die "the board did not come back up as $v"
  wait_for "OTA: trial boot 1 of 3 for $v" 60 "$t" || die "$v booted without its trial bookkeeping"
  say "  running $v on trial"
  wait_for "OTA: $v CONFIRMED" 900 "$t" || die "$v never confirmed"
  say "  $v confirmed"
  wait_for "OTA: rollback copy of $v ready" 420 "$t" || die "$v never saved its rollback copy"
  say "  $v saved as the new rollback copy"
done

save_proof
say "PASS: $BASE_VERSION -> ${NEXT[*]}, every hop over the air, confirmed, with a verified way back"
