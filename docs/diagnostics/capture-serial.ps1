# Capture serial output from the solar tracker into a searchable, timestamped log.
# Windows twin of capture-serial.sh (macOS). Same log format, same redaction rules,
# appends to the same per-day serial-YYYY-MM-DD.log files.
#
# The firmware prints a status block every ten seconds (wind, temperature,
# extension percent, RTC time, button states, tracker status) plus one-off
# messages during boot, WiFi association and actuator moves. Every line gets a
# host timestamp prefix so entries stay correlatable even when the RTC is wrong.
#
# Usage:  powershell -File docs\diagnostics\capture-serial.ps1 [-Port COM3] [-Seconds 0]
# Stop with ctrl-c (or -Seconds N to auto-stop). Safe to re-run; appends to a per-day file.

param(
    [string]$Port,
    # Stop automatically after this many seconds (0 = run until ctrl-c).
    # Exits cleanly so the serial port is always closed properly.
    [int]$Seconds = 0
)

$ErrorActionPreference = 'Stop'

# Auto-detect the UNO R4 WiFi if no port was given. Windows names the port
# "USB Serial Device (COMn)", so key off Arduino's VID_2341/PID_1002 instead.
if (-not $Port) {
    $comPorts = Get-CimInstance Win32_PnPEntity -Filter "Name LIKE '%COM%'"
    $uno = $comPorts | Where-Object { $_.PNPDeviceID -match 'VID_2341&PID_1002' } |
        ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } } |
        Select-Object -First 1
    if (-not $uno) {
        $uno = $comPorts | Where-Object { $_.Name -match 'UNO R4' } |
            ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } } |
            Select-Object -First 1
    }
    if ($uno) { $Port = $uno }
}
if (-not $Port) {
    Write-Error "No Arduino UNO R4 WiFi found on USB. Plug it in and retry (or pass -Port COM#)."
    exit 1
}

$Log = Join-Path $PSScriptRoot ("serial-{0}.log" -f (Get-Date -Format 'yyyy-MM-dd'))
Write-Host "capturing $Port -> $Log"
Write-Host "ctrl-c to stop"

# The dashboard posts WiFi passwords as form parameters, and the firmware echoes
# every parameter it acts on. Redact them on the way in so they never reach disk.
function Redact([string]$line) {
    $line = $line -replace '("wifiPassword"\s*:\s*)"[^"]*"', '$1"[REDACTED]"'
    $line = $line -replace '("pass"\s*:\s*)"[^"]*"', '$1"[REDACTED]"'
    $line = $line -replace '(wifiPassword=)[^&"\s]*', '$1[REDACTED]'
    return $line
}

$serial = New-Object System.IO.Ports.SerialPort $Port, 9600, 'None', 8, 'One'
$serial.NewLine = "`n"
$serial.ReadTimeout = 500
$serial.DtrEnable = $true
$serial.RtsEnable = $true

$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
    $serial.Open()
    while ($true) {
        if ($Seconds -gt 0 -and $sw.Elapsed.TotalSeconds -ge $Seconds) { break }
        try {
            $line = $serial.ReadLine().TrimEnd("`r")
        } catch [System.TimeoutException] {
            continue
        }
        if ($line.Length -eq 0) { continue }
        $stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        $entry = "{0} {1}" -f $stamp, (Redact $line)
        # Anything else reading the log (a tail over SSH, the OTA test harness) can hold it for
        # a moment, and an unhandled write error used to end the capture. Retry instead.
        for ($try = 0; $try -lt 40; $try++) {
            try {
                Add-Content -Path $Log -Value $entry -Encoding UTF8
                break
            } catch [System.IO.IOException] {
                Start-Sleep -Milliseconds 25
            }
        }
        Write-Host $entry
    }
}
finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}
