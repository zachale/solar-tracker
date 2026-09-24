#!/usr/bin/env bash
# Flash a built image over USB on the home PC (ssh host "home"), where the tracker is
# plugged in, and leave the serial capture running afterwards.
#
#   tools/ota/flash-home.sh v1
#
# The board is found by its USB ID (VID_2341&PID_1002), the same way capture-serial.ps1
# finds it, because the COM number is not stable: it moved to COM4 while the bridge was
# in download mode. The capture holds the port, so the capture and its supervisor are
# stopped for the upload, and the supervisor is started again afterwards; it restarts the
# capture within five seconds, well inside the five second pause at the top of setup().
set -euo pipefail

VERSION="${1:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="$HERE/out/$VERSION/solar-tracker.ino.bin"
[[ -f "$BIN" ]] || { echo "no $BIN; run build.sh $VERSION first" >&2; exit 1; }

ssh -o BatchMode=yes home "if not exist \"C:\\Users\\zachl\\ota-flash\\$VERSION\" mkdir \"C:\\Users\\zachl\\ota-flash\\$VERSION\"" >/dev/null
scp -q "$BIN" "home:C:/Users/zachl/ota-flash/$VERSION/solar-tracker.ino.bin"
scp -q "$HERE/capture-supervisor.ps1" "home:C:/Users/zachl/ota-flash/capture-supervisor.ps1"

cat > /tmp/flash-home.ps1 <<PS
\$ErrorActionPreference = 'Stop'
\$repo = 'C:\Users\zachl\Documents\Arduino\solar-tracker'
\$capture = "\$repo\docs\diagnostics\capture-serial.ps1"

\$port = Get-CimInstance Win32_PnPEntity -Filter "Name LIKE '%(COM%'" |
  Where-Object { \$_.PNPDeviceID -match 'VID_2341&PID_1002' -and \$_.Status -eq 'OK' } |
  ForEach-Object { if (\$_.Name -match '\((COM\d+)\)') { \$Matches[1] } } | Select-Object -First 1
if (-not \$port) { Write-Output 'FAIL: no UNO R4 WiFi (VID_2341&PID_1002) on USB'; exit 1 }
Write-Output "board on \$port"

Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" |
  Where-Object { (\$_.CommandLine -like '*capture-supervisor.ps1*' -or \$_.CommandLine -like '*capture-serial.ps1*') -and \$_.ProcessId -ne \$PID } |
  ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }
Start-Sleep -Seconds 2

Set-Location \$repo
\$ErrorActionPreference = 'Continue'
\$out = & arduino-cli upload -b arduino:renesas_uno:unor4wifi -p \$port --input-dir 'C:\Users\zachl\ota-flash\\$VERSION' . 2>&1
\$code = \$LASTEXITCODE
\$out | Where-Object { \$_ -match 'Done in|rror|Write ' }
\$uploadFailed = \$code -ne 0
if (\$uploadFailed) { Write-Output "FAIL: upload exited \$code" }

# Restarted whether or not the upload worked, so a failed flash never leaves the board
# unlogged. Launched through WMI rather than Start-Process: Windows OpenSSH kills
# everything a session started when the session ends, and a WMI-created process is
# outside that job.
\$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
  CommandLine = 'powershell -NoProfile -ExecutionPolicy Bypass -File "C:\Users\zachl\ota-flash\capture-supervisor.ps1"'
  CurrentDirectory = \$repo }
Start-Sleep -Seconds 8
\$cap = Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" | Where-Object { \$_.CommandLine -like '*capture-serial.ps1*' }
if (\$cap) { "capture running under supervisor \$(\$r.ProcessId), capture PID \$(\$cap.ProcessId)" }
else { Write-Output 'FAIL: supervisor did not start a capture'; exit 1 }
if (\$uploadFailed) { exit 1 }
PS
scp -q /tmp/flash-home.ps1 "home:C:/Users/zachl/flash-home.ps1"
ssh -o BatchMode=yes home 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\zachl\flash-home.ps1'
