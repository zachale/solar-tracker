# Diagnostics

Field data captured from the deployed tracker. Kept in the repo because the
sources it comes from are transient.

## Files

| File | What it is |
| --- | --- |
| `capture-serial.sh` | Streams the R4's serial output to a timestamped, per-day log |
| `serial-YYYY-MM-DD.log` | Serial captures. Host UTC timestamp, then the firmware's own line |
| `cloudrun-requests-*.json` | Raw Cloud Run request records for the API |
| `cloudrun-requests-*.tsv` | The same, flattened, with an EDT column |

## Why these are committed

Cloud Logging's default bucket retains 30 days. The only recorded evidence of
the August 2026 morning freeze — the boot at 2026-08-21 20:57 EDT and the failed
08:00 sync the next morning — would have aged out on 2026-09-21.

Serial output is not recorded anywhere by the device itself. If it is not
captured while a laptop is attached, it is gone.

## Reading a serial capture

The firmware prints a status block every ten seconds:

```
Wind Speed: 1.00
Temperature: 22.25
PercentExtended: 29
9:51:18                                    <- RTC wall clock
1788947478                                 <- RTC as a unix timestamp
Settings Server: 1 Wind Sensor: 1 Safe Mode Override: 0
Status: ACTIVE
```

`Settings Server: 1` means the pin D7 switch is in the settings position. In
that state the radio runs as an access point named `Solar-Tracker`, so the
device cannot reach the internet, and `pollSensorData()` explicitly skips the
daily clock sync.

Useful greps:

```bash
grep -c "Failed to connect to WiFi"      serial-*.log   # association failures
grep -n  "boundary hit\|recalibrat"      serial-*.log   # actuator stalls
grep -n  "Extending to"                  serial-*.log   # every commanded move
grep -n  "Response code\|Request Failed" serial-*.log   # HTTP outcomes
awk '/PercentExtended/{print $1, $3}'    serial-*.log   # position over time
```
