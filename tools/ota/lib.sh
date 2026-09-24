# Shared by prove.sh and verify.sh. Source it; do not run it.
#
# Everything here talks to the tracker through the home PC (ssh host "home"): USB flashing,
# the serial capture's log, and the board's presence on USB. Every wait counts only lines
# logged after its step began, because a line left over from an earlier boot once started
# an upload on top of a restarting bridge.

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUCKET="solar-tracker-442903-ota"
TOKEN="$(cat "$HERE/.ota_token")"

say() { printf '%s  %s\n' "$(date -u +%H:%M:%SZ)" "$*"; }
now() { date -u +%Y-%m-%dT%H:%M:%SZ; }

# Helpers run on the PC from script files: quotes do not survive bash -> ssh -> cmd ->
# PowerShell intact when inlined.
cat > /tmp/ota-board-present.ps1 <<'PS'
$b = Get-CimInstance Win32_PnPEntity -Filter "Name LIKE '%(COM%'" |
  Where-Object { $_.PNPDeviceID -match 'VID_2341&PID_1002' -and $_.Status -eq 'OK' }
if ($b) { 'yes' }
PS
# The capture starts a new file at local midnight, so the two newest are read in order.
cat > /tmp/ota-log-tail.ps1 <<'PS'
Get-ChildItem 'C:\Users\zachl\Documents\Arduino\solar-tracker\docs\diagnostics\serial-*.log' |
  Sort-Object LastWriteTime -Descending | Select-Object -First 2 | Sort-Object LastWriteTime |
  ForEach-Object { Get-Content $_.FullName -Tail 4000 }
PS
scp -q /tmp/ota-board-present.ps1 'home:C:/Users/zachl/ota-flash/board-present.ps1'
scp -q /tmp/ota-log-tail.ps1 'home:C:/Users/zachl/ota-flash/log-tail.ps1'

board_present() {
  ssh -o BatchMode=yes home 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\zachl\ota-flash\board-present.ps1' 2>/dev/null | grep -q yes
}

lines_since() {
  ssh -o BatchMode=yes home 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\zachl\ota-flash\log-tail.ps1' \
    2>/dev/null | tr -d '\r' | awk -v t="$1" '$1 >= t'
}

# wait_for <regex> <seconds> <since> [strict]
# 0 = seen. 1 = timed out. 2 = an OTA failure was logged first (only in strict mode, the
# default; pass "lenient" where a failure is the expected outcome).
wait_for() {
  local pattern="$1" timeout="$2" since="$3" mode="${4:-strict}" deadline out
  deadline=$((SECONDS + timeout))
  while ((SECONDS < deadline)); do
    out="$(lines_since "$since")"
    if grep -qE "$pattern" <<<"$out"; then return 0; fi
    if [[ "$mode" == strict ]] && grep -qE "OTA: .* failed \(|CANNOT ROLL BACK" <<<"$out"; then
      grep -E "OTA: .* failed \(|CANNOT ROLL BACK" <<<"$out" | tail -1
      return 2
    fi
    sleep 5
  done
  return 1
}

# Count lines matching <regex> logged since <since>.
count_since() { lines_since "$2" | grep -cE "$1"; }

# USB flash through flash-home.sh, failing on anything but a clean upload with the capture
# restarted. Not `flash-home | grep FAIL && die`: under pipefail a failed flash makes that
# pipeline non-zero even when grep matches, and the die never runs.
flash() {
  local out
  out="$("$HERE/flash-home.sh" "$1" 2>&1)"
  if [[ $? -ne 0 ]] || grep -q "FAIL" <<<"$out"; then
    printf '%s\n' "$out" | grep -E "FAIL|rror" | head -3
    return 1
  fi
}

# The manifest is "<version> <bytes> <crc32>" of the decompressed image. For a version that
# was never built (the missing-image test) size and CRC are zero.
manifest_line() {
  local bin="$HERE/out/$1/solar-tracker.ino.bin"
  if [[ -f "$bin" ]]; then
    python3 -c 'import sys,zlib; d=open(sys.argv[2],"rb").read(); print(sys.argv[1], len(d), format(zlib.crc32(d),"08x"))' "$1" "$bin"
  else
    echo "$1 0 0"
  fi
}

set_manifest_line() {
  printf '%s\n' "$1" > "$HERE/out/manifest.txt"
  gcloud storage cp "$HERE/out/manifest.txt" "gs://$BUCKET/$TOKEN/manifest.txt" \
    --cache-control=no-store --content-type=text/plain --quiet >/dev/null 2>&1
  [[ "$(curl -fsS "https://storage.googleapis.com/$BUCKET/$TOKEN/manifest.txt")" == "$1" ]]
}

set_manifest() { set_manifest_line "$(manifest_line "$1")"; }

clear_manifest() {
  gcloud storage rm "gs://$BUCKET/$TOKEN/manifest.txt" --quiet >/dev/null 2>&1
  ! curl -fsS -o /dev/null "https://storage.googleapis.com/$BUCKET/$TOKEN/manifest.txt" 2>/dev/null
}

# Serial lines since <since>, less the ten second status noise, for keeping as evidence.
evidence_since() {
  lines_since "$1" | grep -vE "^[^ ]+ (Wind Speed|PercentExtended|Settings Server|[0-9]+:[0-9]+:[0-9]+$|[0-9]{10}$)"
}
