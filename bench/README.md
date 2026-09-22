# Bench tests

Non-destructive stress tests for the tracker board with only the DS3231 and the
SD card attached. No actuator, no PSU, no anemometer.

Goal: find what combination of clock reads, SD writes and radio work makes the
DS3231 stop answering on I²C.

## Safety rules these sketches follow

- **No sketch includes `LinearActuator`.** Every one drives D5 and D6 low at boot
  so a reconnected driver could not be commanded even by accident.
- **No sketch deletes a file it did not create.** `benchSafeRemove()` refuses any
  name not starting with `bench`, so `tlm*.csv`, `config` and `wifi.jsn` are safe.
- **`01_seed_wifi` backs up `wifi.jsn` to `wifi.bak` and verifies the backup**
  before modifying anything, and restores it if the new file fails to parse.
- **Passwords are never printed**, and `run.sh` redacts them again on the way to disk.
- Test data goes to `bench0N.csv`, capped at ~200 KB.

## Run order

| # | Sketch | What it establishes |
|---|---|---|
| 00 | `00_card_inventory` | What is on the card and how many telemetry rows are still waiting to upload. **Writes nothing.** Run this first. |
| 01 | `01_seed_wifi` | Appends the new SSID to `wifi.jsn`, keeping existing entries |
| 02 | `02_baseline_rtc` | Control: RTC alone at 10 Hz. If it fails here, nothing else is implicated |
| 03 | `03_sd_stress` | RTC + SD writes, period ramping 500 → 30 ms |
| 04 | `04_wifi_stress` | RTC + associate / TLS GET / `WiFi.end()` cycles |
| 05 | `05_combined` | All three at once, the field workload at ~100× rate |
| 06 | `06_recovery_ladder` | Stress until wedge, then find which recovery step actually works |

```
./bench/run.sh 00_card_inventory
./bench/run.sh 03_sd_stress 600
```

Logs land in `bench/logs/<sketch>-<timestamp>.log`, host-timestamped.

## Reading the output

Every observation is one line:

```
BENCH 03 n=412 ms=94820 probe=0 osf=0 unix=1789... ymd=2026-09-18 hms=14:22:01 temp=21.75 ram=19""" bad=0 EVENT=...
```

- `probe` — raw `Wire.endTransmission()` to 0x68, bypassing RTClib.
  `0` ok · `2` NACK on address · `3` NACK on data · `5` timeout
- `osf` — the DS3231's oscillator-stop flag. **1 means the chip genuinely lost
  power**; 0 with a bad read means the chip is fine and the bus failed. This is
  the bit the shipped firmware never looks at outside `setup()`.
- `temp=17.00` — not a temperature. It is register address `0x11` left in
  RTClib's buffer by a failed transaction.
- `bad=1` — year implausible, or probe non-zero, or temp stuck at 17.00.

## Server writes

`04` and `05` do a read-only `GET /time/est`. Neither posts telemetry, so the
production bucket is untouched. `04_wifi_stress` has `BENCH_ALLOW_POST` if that
ever needs to change.

## Restoring the tracker

```
arduino-cli compile -u -p <port> --fqbn arduino:renesas_uno:unor4wifi solar-tracker.ino
```

---

# Findings — 2026-09-18

Bench configuration: UNO R4 WiFi, DS3231 on D8/A4/A5 with a coin cell fitted,
microSD on SPI. **No actuator, no 12 V PSU, no motor driver, no anemometer.**
Wind-sensor switch disabled. No sketch compiles `LinearActuator`; D5/D6 driven
low at boot.

## Result: the fault did not reproduce. Not once.

| Test | Duration | RTC samples | SD writes | Radio cycles | Wedges | Anomalies |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 02 baseline (RTC only, 10 Hz) | 201 s | 1,924 | — | — | 0 | **0** |
| 03 SD stress (500 → 30 ms) | 341 s | 3,431 | 3,431 | — | 0 | **0** |
| 04 WiFi stress (assoc/TLS/end) | 192 s | 2,628 | — | 6 | 0 | **0** |
| 05 combined (all three) | 274 s | 1,665 | 157 | 7 | 0 | **0** |
| 06 recovery ladder | 384 s | 2,380 | — | — | **0** | **0** |
| 07 drain (527 rows, 5 POSTs) | 64 s | — | — | 5 POST | 0 | **0** |
| **Total** | **~25 min** | **12,031** | **3,588** | **18** | **0** | **0** |

Every sample carried `probe=0` (raw `Wire.endTransmission()` to 0x68), `osf=0`
(oscillator never stopped), a real temperature, and a correct date. Free RAM was
flat in every test — no leak in the spool or upload path.

## What this rules out

- **SD writes alone.** 3,431 writes at up to 33/s, each immediately following an
  I²C read, which is the firmware's exact `spoolTelemetryRow()` pattern at roughly
  20,000× field cadence. Zero anomalies, zero write failures.
- **`WiFi.end()` and radio teardown.** The call `71b63aa` added and the last
  known-good build never made, exercised 18 times with the RTC sampled every
  20 ms across the transient and the 3 s after it. Nothing.
- **I²C read frequency.** 10 Hz sustained, ~60× the firmware's rate.
- **Everything at once.** Test 05 ran all three concurrently for five minutes.
- **Memory pressure.** RAM never moved; the earlier heap-fragmentation theory was
  already disproven in source and is now disproven on hardware.

## What is left

The bench differs from the field unit in exactly three things: the **12 V PSU**,
the **BTS7960 driver**, and the **linear actuator**. All three share the supply
and ground with the RTC, and the actuator is the only component in the system
that draws amps.

Every captured failure in `docs/diagnostics/` came from a `BENCH_TEST=1` build
with actuator movement compiled out, so motion was never tested — and now
everything *except* motion has been tested clean. The remaining hypothesis is
motor current, not code.

## Incidental findings

1. **`Rock Lodge` never associates.** It is first in `wifi.jsn`, so every sync
   burns a full association timeout before falling through to the working SSID.
   Reorder or remove it.
2. **Marginal signal drops TLS requests intermittently.** One `GET connect FAILED`
   at −87 dBm in test 04, recovering on the next cycle. When that happens to a
   POST, `uploadLogs()` breaks and keeps the chunk and the server sees nothing —
   indistinguishable from "nothing to upload". This is a plausible mechanism for
   the 527 stranded rows.
3. **`R4HttpClient` returns `-1` for everything.** No signal, bad URL, TLS
   timeout — all `-1`. Test 08 exists because of this: it walks L1 association →
   L2 DHCP → L3 DNS → L4 TLS → L5 HTTP and reports each separately.
4. **527 rows were stranded on the card and are now uploaded** (test 07), all
   five chunks confirmed 200 and cleared only after. `raw/2026-09-18/`.

## Next experiment

Reconnect the 12 V PSU and the driver, keep the actuator disconnected, and run
05. Then reconnect the actuator and run it again. That isolates "supply present"
from "motor moving" — the only two variables left untested.
