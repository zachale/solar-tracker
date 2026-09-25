#!/usr/bin/env bash
# The OTA fault matrix, run on the bench tracker at the home PC.
#
#   tools/ota/verify.sh [--hops N] [--skip-hang] [--only <scenario>]
#
# Builds every image it needs with fast timers (check every 60 s, confirm after 60 s, roll
# back after 300 s; the logic is the production logic), then drives the board through:
#
#   base       USB-flash the first image; it saves a copy of itself on the SD card
#   hops       normal updates, each confirmed after its trial        (N, default 3)
#   noconfirm  an image that never confirms           -> rolled back at the deadline
#   crash      an image that hard-faults in setup()   -> watchdog resets, rolled back on boot 4
#   hang       an image that hangs in setup()         -> watchdog resets, rolled back on boot 4
#   abort      a good image, manifest pointed back mid-trial -> rolled back at once
#   corrupt    payload byte flipped                   -> refused at verify, never flashed
#   magic      another board's magic number           -> refused, never flashed
#   readback   manifest gives the wrong CRC           -> read back, refused, never flashed
#   missing    manifest names an image not uploaded   -> refused before the bridge sees it
#   recover    one more normal update after all of it -> still accepted
#
# Every scenario ends with the board on a known version and the evidence in
# tools/ota/proofs/. A scenario fails fast and stops the run; nothing after it is claimed.
set -uo pipefail
source "$(cd "$(dirname "$0")" && pwd)/lib.sh"

HOPS=3
SKIP_HANG=0
ONLY=""
while [[ "${1:-}" == --* ]]; do
  case "$1" in
    --hops) HOPS="$2"; shift ;;
    --skip-hang) SKIP_HANG=1 ;;
    --only) ONLY="$2"; shift ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
  shift
done

RUN="q$(date -u +%d%H%M)"            # names are never reused, so every run gets fresh ones
RESULTS=()
GOOD=""                              # the version the board should be running now
RETRIES=0
RUN_START="$(now)"
mkdir -p "$HERE/proofs"
PROOF="$HERE/proofs/$(date -u +%Y%m%dT%H%M%SZ)-matrix-$RUN.log"
SUMMARY="${PROOF%.log}.summary.txt"

record() { RESULTS+=("$1"); say "$1"; }
finish() {
  evidence_since "$RUN_START" > "$PROOF"
  { printf 'OTA fault matrix, run %s, %s\n\n' "$RUN" "$(date -u)"; printf '%s\n' "${RESULTS[@]}"; } > "$SUMMARY"
  say "evidence: ${PROOF#$HERE/}"
  say "summary:  ${SUMMARY#$HERE/}"
}
die() { record "FAIL  $*"; finish; exit 1; }
want() { [[ -z "$ONLY" || "$ONLY" == "$1" ]]; }

v() { echo "$RUN$1"; }               # q251742a, q251742nc, ...

# ---------------------------------------------------------------------------------------------
say "building images for run $RUN"
build() { "$HERE/build.sh" --fast "$@" 2>&1 | tail -1 | grep -q "^built" || die "build $*"; }
NORMAL=(a b c d e)
for ((i = 0; i < HOPS; i++)); do NORMAL+=("h$i"); done
for t in "${NORMAL[@]}"; do build "$(v "$t")"; done
build --fault=no-confirm "$(v nc)"
build --fault=crash-setup "$(v cr)"
build --fault=hang-setup "$(v hg)"
build "$(v bad)"
build "$(v mg)"
build "$(v rb)"

# corrupt: one payload byte flipped, header left as it was, so the CRC cannot match
python3 - "$HERE/out/$(v bad)/$(v bad).ota" <<'PY'
import sys
p = sys.argv[1]; d = bytearray(open(p, 'rb').read()); d[20 + 1000] ^= 0xFF; open(p, 'wb').write(d)
PY
# magic: another board's magic number, CRC recomputed so only the magic is wrong
python3 - "$HERE/out/$(v mg)/$(v mg).ota" <<'PY'
import sys, struct, zlib
p = sys.argv[1]; d = bytearray(open(p, 'rb').read())
d[8:12] = struct.pack('<I', 0x23418057)            # NANO_33_IOT
d[4:8] = struct.pack('<I', zlib.crc32(bytes(d[8:])))
open(p, 'wb').write(d)
PY
for t in bad mg; do echo "TEST IMAGE: $t" > "$HERE/out/$(v "$t")/TEST-IMAGE"; done

say "uploading every image except the one the missing test must not find"
for t in "${NORMAL[@]}" nc cr hg bad mg rb; do
  "$HERE/publish.sh" --no-manifest --test "$(v "$t")" >/dev/null 2>&1 || die "upload $(v "$t")"
done

say "waiting for the board on USB"
until board_present; do sleep 10; done

# ---------------------------------------------------------------------------------------------
# accepted <version>: the board installs it, runs its trial, confirms it and makes it the
# rollback copy.
accepted() {
  local next="$1" t
  t="$(now)"
  set_manifest "$next" || die "set manifest $next"
  # A download can fail on the way (a network stall, once, on this unit); the board retries by
  # itself, up to three times, and that is part of what is being shown. So failures are counted
  # and reported, not fatal. A rollback, or a board that never installs, still is.
  wait_for "OTA: image verified, installing $next" 600 "$t" lenient || die "$next was never installed"
  RETRIES="$(count_since "OTA: .* failed \(" "$t")"
  wait_for "Firmware version: $next" 120 "$t" || die "the board did not come back as $next"
  wait_for "OTA: trial boot 1 of 3 for $next" 60 "$t" || die "$next booted without its trial bookkeeping"
  wait_for "OTA: $next CONFIRMED" 300 "$t" || die "$next never confirmed"
  wait_for "OTA: rollback copy of $next ready" 240 "$t" || die "$next never saved its rollback copy"
  if (( $(count_since "ROLLING BACK|CANNOT ROLL BACK" "$t") > 0 )); then die "$next was rolled back"; fi
  GOOD="$next"
}
retried() { (( RETRIES > 0 )) && echo ", after $RETRIES failed download(s) retried by itself"; }

# rolled_back <bad> <reason regex> <seconds>: the board installs <bad>, which fails its trial,
# and ends up back on the good version with <bad> rejected. The copy on the SD card is only
# read by a rollback, so it still stands and nothing is saved again.
rolled_back() {
  local bad="$1" reason="$2" timeout="$3" t
  t="$(now)"
  set_manifest "$bad" || die "set manifest $bad"
  wait_for "OTA: image verified, installing $bad" 600 "$t" lenient || die "$bad was never installed"
  wait_for "OTA: ROLLING BACK to $GOOD from the SD card: $reason" "$timeout" "$t" lenient || die "$bad was not rolled back ($reason)"
  wait_for "OTA: copy of $GOOD written to the ESP32 and read back identical" 420 "$t" lenient || die "the copy of $GOOD never reached the ESP32"
  wait_for "OTA: ROLLED BACK from $bad to $GOOD" 180 "$t" lenient || die "the board did not come back as $GOOD"
  wait_for "OTA: manifest wants $bad, which failed its trial" 180 "$t" lenient || die "$bad was not held as rejected"
}

# refused <version> <failure regex>: the board tries <version>, the attempt fails before
# anything is flashed, and it never restarts.
refused() { refused_line "$1" "$(manifest_line "$1")" "$2"; }
refused_line() {
  local bad="$1" line="$2" failure="$3" t
  t="$(now)"
  set_manifest_line "$line" || die "set manifest $bad"
  wait_for "$failure" 300 "$t" lenient || die "$bad was not refused as expected"
  wait_for "failed [0-9]+ times; not trying again|attempt 3 of 3" 420 "$t" lenient || die "$bad kept being retried"
  if (( $(count_since "Firmware version:" "$t") > 0 )); then die "the board restarted while refusing $bad"; fi
  if (( $(count_since "installing $bad" "$t") > 0 )); then die "$bad got as far as installing"; fi
  # A retry after a failed download used to crash the bridge; it is restarted cleanly instead.
  if (( $(count_since "startDownload failed \(-26\)" "$t") > 0 )); then die "the bridge crashed on a retry of $bad"; fi
}

# ---------------------------------------------------------------------------------------------
if want base; then
  t="$(now)"
  set_manifest "$(v a)" || die "set manifest"
  flash "$(v a)" || die "USB flash of $(v a)"
  wait_for "Firmware version: $(v a)" 90 "$t" || die "$(v a) did not boot"
  wait_for "Watchdog armed" 30 "$t" || die "the watchdog did not arm"
  wait_for "OTA: rollback copy of $(v a) ready" 240 "$t" || die "$(v a) never saved its rollback copy"
  wait_for "OTA: up to date at $(v a)" 180 "$t" || die "$(v a) did not report up to date"
  GOOD="$(v a)"
  record "PASS  base      $(v a) flashed over USB, watchdog armed, copy of itself saved on the SD card"
fi
[[ -n "$GOOD" ]] || die "no base; run without --only first"

if want hops; then
  for t in b c "${NORMAL[@]:5}"; do
    accepted "$(v "$t")"
    record "PASS  hop       installed $(v "$t") over the air, trial confirmed, rollback copy saved$(retried)"
  done
fi

if want noconfirm; then
  rolled_back "$(v nc)" "did not confirm" 600
  record "PASS  noconfirm $(v nc) never confirmed and was rolled back to $GOOD at the deadline; rejected"
fi

if want crash; then
  rolled_back "$(v cr)" "booted 4 times without confirming" 300
  record "PASS  crash     $(v cr) hard-faulted every boot; watchdog reset it; rolled back to $GOOD on boot 4"
fi

if want hang && (( ! SKIP_HANG )); then
  rolled_back "$(v hg)" "booted 4 times without confirming" 1200
  record "PASS  hang      $(v hg) hung in setup every boot; watchdog reset it; rolled back to $GOOD on boot 4"
fi

if want abort; then
  t="$(now)"
  set_manifest "$(v d)" || die "set manifest"
  wait_for "OTA: trial boot 1 of 3 for $(v d)" 300 "$t" || die "$(v d) never started its trial"
  set_manifest "$GOOD" || die "set manifest back"
  wait_for "OTA: ROLLING BACK to $GOOD from the SD card: the manifest points back to $GOOD" 180 "$t" lenient || die "the abort did not roll back"
  wait_for "OTA: copy of $GOOD written to the ESP32 and read back identical" 420 "$t" lenient || die "the copy of $GOOD never reached the ESP32"
  wait_for "OTA: ROLLED BACK from $(v d) to $GOOD" 180 "$t" lenient || die "the board did not come back as $GOOD"
  record "PASS  abort     manifest pointed back mid-trial; rolled back from $(v d) to $GOOD from the SD card, no download"
fi

if want corrupt; then
  refused "$(v bad)" "OTA: (verify|downloadProgress|download stalled[^(]*) failed"
  record "PASS  corrupt   $(v bad) with a flipped byte failed its CRC, three tries, never flashed"
fi

if want magic; then
  # The bridge crashes on this download, which drops USB for a few seconds, so the line saying
  # so can fall in the capture's gap. Its giving up after three is logged once USB is back.
  refused "$(v mg)" "OTA: (downloadProgress|verify) failed|OTA: $(v mg) failed 3 times; not trying again"
  record "PASS  magic     $(v mg) with another board's magic was refused, never flashed"
fi

if want readback; then
  # A sound image whose manifest gives the wrong CRC: the download and its transfer CRC pass,
  # the read-back does not, and nothing is flashed.
  refused_line "$(v rb)" "$(v rb) $(manifest_line "$(v rb)" | awk '{print $2}') deadbeef" "OTA: read-back of the new image failed"
  record "PASS  readback  $(v rb) with a wrong CRC in the manifest was read back, refused, never flashed"
fi

if want missing; then
  refused "$(v zz)" "OTA: probe of the image failed \(404\)"
  record "PASS  missing   manifest named an image that does not exist; found missing before the bridge was asked, never flashed, no restart"
fi

if want recover; then
  accepted "$(v e)"
  record "PASS  recover   after every failure above, $(v e) still installs and confirms normally$(retried)"
fi

set_manifest "$GOOD" >/dev/null
finish
say "ALL PASS: ${#RESULTS[@]} scenarios; the board is on $GOOD and the manifest agrees"
