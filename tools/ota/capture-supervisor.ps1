# Keep a serial capture running on the home PC for as long as the tracker is plugged in.
#
# A USB drop kills the capture: a replug, a bridge restart, the ESP32 going into download
# mode. This checks every five seconds and starts capture-serial.ps1 again whenever the
# board is present and nothing is logging it. flash-home.sh stops this and the capture
# before an upload and starts this again afterwards, so the two never fight over the port.
#
# Launch it through WMI (Win32_Process.Create), not from an SSH session directly: Windows
# OpenSSH kills everything a session started when the session ends.

$repo = 'C:\Users\zachl\Documents\Arduino\solar-tracker'
$capture = "$repo\docs\diagnostics\capture-serial.ps1"

while ($true) {
    $port = Get-CimInstance Win32_PnPEntity -Filter "Name LIKE '%(COM%'" |
        Where-Object { $_.PNPDeviceID -match 'VID_2341&PID_1002' -and $_.Status -eq 'OK' } |
        ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } } | Select-Object -First 1

    $running = Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" |
        Where-Object { $_.CommandLine -like '*capture-serial.ps1*' }

    if ($port -and -not $running) {
        Start-Process -FilePath powershell -WindowStyle Hidden -WorkingDirectory $repo `
            -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $capture, '-Port', $port)
    }
    Start-Sleep -Seconds 5
}
