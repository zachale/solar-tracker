# 07 — A real diagnostics channel in the telemetry schema

**Status:** Proposed
**Owner:** unassigned
**Depends on:** nothing
**Blocks:** 02 (the OTA update trigger compares a target version against the version the
tracker reports, and today the tracker reports nothing)
**Estimate:** 2–2.5 days (13–19 h): server 4–5 h, firmware 5–8 h, bench 3–4 h, docs 1 h
**Cost:** $0.00/month. Row grows from 33.4 B to ~76 B, so 144 rows/day = 10.9 KB/day =
0.33 MB/month of NDJSON. At us-central1 Standard $0.020/GB-month that is
0.00033 GB × $0.020 = $0.0000066/month, and it sits inside the 5 GB-month free tier.
Object count and request count do not change at all, so the ~1,460 Class A writes/month
stay under the 5,000/month free tier. Cloud Run request count is unchanged.

## Problem

Wedge counts reach the cloud disguised as wind speed. `spoolTelemetryRow()` picks between
real wind data and `ClockModule::wedgeCount` with a ternary on a physical switch
(`src/SolarTracker/SolarTracker.cpp:154-156`), clamped to 99 because the field was sized for
wind, and the selector is `digitalRead(D4) == HIGH` — so a floating D4 silently reverts the
column to wind and the wedge signal disappears with no indication anywhere. The reason for
the disguise is real: the server accepts exactly 8 CSV columns from a fixed status enum
(`server/routes.js:28-56` on `feature/telemetry-api`) and answers 200 for a batch it stored
nothing from (`routes.js:126-127`). The firmware then deletes the chunk, because
`WifiClient::post()` returns only the status code and throws away `http.getBody()`
(`src/WifiClient/WifiClient.cpp:122-125`), and `uploadLogs()` treats any 200 as proof of
storage (`src/SolarTracker/SolarTracker.cpp:295-301`). The card is the only copy, so a
schema mismatch does not merely go unnoticed — it destroys the data it fails to store.

## Goal

Every telemetry row carries its own schema version and real columns for wedge count,
recover count, last I2C code, free RAM, reset cause and firmware version; and a row the
server cannot parse is counted, logged and archived rather than dropped.

## Non-goals

- Does not change when `ClockModule::isHealthy()` is called. It still runs on every
  `loop()` iteration via `solar-tracker.ino:95` → `SolarTracker.cpp:37`. This PRD only
  reports counters that code already maintains.
- Does not fix the year-2043 hole in `isHealthy()` (`src/Clock/Clock.cpp:24` accepts any
  year in 2020..2099, so a garbage read can pass). Until that is fixed, `last_i2c_code`
  shows only the wedges the address probe catches.
- Does not add a general logging pipeline, a second endpoint, or Cloud Logging ingestion.
  One CSV row per 10 minutes over the existing `POST /data` is the whole transport.
- Does not add a watchdog, and does not use the new `reset_cause` column to trigger
  anything on the device. It is reported, not acted on.
- Does not change `chunkBytes`, the upload cadence, the AP-mode rules, or anything about
  how the actuator moves.
- Does not touch `main`. The server work lands on `feature/telemetry-api`, which is what is
  deployed.

## Requirements

1. Prefix every spooled row with a literal schema tag (`v2`) so the parser dispatches on a
   declared version rather than inferring one from column count.
2. Extend the row with `wedge_count`, `recover_count`, `last_i2c_code`, `free_ram`,
   `reset_cause`, `boot_id`, `uptime_s` and `firmware_version`, in that order after the
   existing eight fields.
3. Return `wind_peak` to carrying wind speed unconditionally, and delete the
   `ButtonPanel::isWindSensorEnabled()` selector and the 99-count clamp from
   `spoolTelemetryRow()`.
4. Report `wedge_count` and `recover_count` at their full `uint16_t` range, since both are
   per-power-session counters (`src/Clock/Clock.h:34-35`).
5. Report `free_ram` using the existing `freeRam()` helper from
   `bench/common/bench_common.h:23-29`, so the field is the same number the bench logs
   already print (21,700 B measured in the SD stress run) and the two are comparable.
6. Read `reset_cause` from the RA4M1 reset status registers once in `setup()`, before
   anything else can clear them, and emit the raw register bytes as hex rather than a
   decoded label.
7. Emit a `boot_id` that distinguishes one power session from the next, so per-session
   counters can be diffed without mistaking a reboot for a quiet hour.
8. Emit `uptime_s` from `millis()/1000` so rows stay orderable when the RTC stamps them
   with its power-on epoch — verified in this repo: `bench/logs/00_card_inventory-20260918T114838.log:19`
   shows `tlm0.csv bytes=4072 rows=122 first_unix=946684800`, i.e. a whole chunk stamped
   2000-01-01.
9. Emit `firmware_version` from a single build-time constant that is also the token used to
   name the `.ota` object, because PRD 02's trigger compares the two
   (`docs/ota-esp32-bridge.md`, "Trigger: pull only, no push", items 1 and 3).
10. Accept both the 8-column v1 row and the tagged v2 row in `parseRow()` for the duration
    of the spool drain, and keep v1 acceptance until the server has seen zero v1 rows for a
    week.
11. Count every rejected row by reason (`width`, `schema`, `time`, `status`, `number`,
    `flags`) and return those counts in the `POST /data` response body.
12. Log one structured `severity: WARNING` line per reject reason per batch, capped at five
    sample lines per batch so a pathological upload cannot flood the log.
13. Write any batch with `saved === 0` to `gs://solar-tracker-442903-telemetry/rejected/<day>/<stamp>.csv`
    before answering, so the bytes survive the chunk deletion that follows.
14. Keep answering 200 on an unparseable batch. A 4xx would make `uploadLogs()` break out
    and keep the chunk (`SolarTracker.cpp:295-299`), and the ring would then wrap and clear
    the oldest chunk to make room (`Telemetry.cpp:97-104`), losing newer rows to protect a
    bad one.
15. Capture the POST response body in `WifiClient` via `R4HttpClient::getBody()` and print
    a warning line whenever `saved` is 0 or `skipped` is non-zero, so a mismatch is visible
    on serial as well as in the cloud.
16. Decode `reset_cause` bits to names on the server, not on the device, so the decode table
    can change without a flash.
17. Keep the compiled sketch under 135,000 bytes of flash and add no more than 64 bytes of
    globals, measured with `arduino-cli compile` against the 127,688 B / 11,212 B baseline.
18. Raise `Telemetry::maxChunks` to 128 to hold the outage-tolerance window at roughly 47
    days, since a ~76 B row cuts a 4,096 B chunk from 122 rows to ~53.
19. Update `SPEC.md` FR-34, which still documents telemetry as a MongoDB `datalogs`
    document (`SPEC.md:173,178`) — that server is dead.

## Design

### Order of work — this ordering is the fail-safe part

1. **Server first.** Deploy a `parseRow()` that accepts v1 and v2. An updated server with
   old firmware is harmless; new firmware against the old server loses every row it sends.
2. **Verify with curl** against the deployed URL before touching the board (matrix below).
3. **Firmware on the bench**, `BENCH_TEST` build, with `chunkBytes = 256` so several chunks
   fill in minutes and the multi-chunk upload path actually runs.
4. **Confirm the rows landed** in `gs://solar-tracker-442903-telemetry/raw/` with `diag`
   populated.
5. **Only then flash the field unit.** No unbuilt path is ever the thing in the field.

Rollback is asymmetric on purpose: the server is a Cloud Run revision rollback, the
firmware is a trip. So nothing on the device depends on a server change landing.

### The row

```
v2,1788947478,18.25,35,ACTIVE,3,0,0,0,8,8,2,21556,00040101,00a3,54321,1.4.0
```

| # | Field | Source | Notes |
| --- | --- | --- | --- |
| 1 | `schema` | literal `v2` | dispatch tag |
| 2 | `time` | `clockModule.getTimestamp()` | unchanged |
| 3 | `temperature` | `clockModule.getClockTemp()` | unchanged. `17.00` here is already the reliable tell for a failed `getTemperature()` — worth querying today, no new field needed |
| 4 | `extended_percent` | `actuator.getPercentExtended()` | unchanged |
| 5 | `status` | `getStatusString()` | unchanged enum |
| 6 | `wind_peak` | `windPeak` | wind only, no selector |
| 7-9 | `settings`,`wind_sensor`,`away` | `ButtonPanel` | unchanged |
| 10 | `wedge_count` | `ClockModule::wedgeCount` | full `uint16_t`, per power session |
| 11 | `recover_count` | `ClockModule::recoverCount` | full `uint16_t` |
| 12 | `last_i2c_code` | `ClockModule::lastI2cCode` | `Wire.endTransmission()` code: 0 ok, 2 address NACK, 5 timeout |
| 13 | `free_ram` | `freeRam()` | newlib `sbrk` against the stack pointer |
| 14 | `reset_cause` | `RSTSR1`,`RSTSR0`,`RSTSR2` | 8 hex chars, raw |
| 15 | `boot_id` | boot nonce | 4 hex chars |
| 16 | `uptime_s` | `millis()/1000` | monotonic ordering key |
| 17 | `firmware_version` | `FIRMWARE_VERSION` | must not contain a comma; keep ≤ 7 chars |

Measured effects, against the 33.4 B/row on the card today
(`bench/logs/00_card_inventory-20260918T114838.log:19`, 4,072 B / 122 rows):

| | v1 | v2 |
| --- | --- | --- |
| Typical row | 33.4 B | ~76 B |
| Rows per 4,096 B chunk | 122 | ~53 |
| Outage tolerance, 64 chunks | ~50 days | ~24 days |
| Outage tolerance, 128 chunks | — | ~47 days |
| Bytes/day at 144 rows | 4.8 KB | 10.9 KB |
| Peak RAM per upload | unchanged | unchanged |

Peak RAM does not move because chunking is by bytes, not rows: `readChunk()` still builds one
~4 KB `String` (`Telemetry.cpp:125-155`). Only the row count per request falls. Note the
24-day figure is outage tolerance, not normal cadence — `uploadLogs()` drains every non-empty
chunk including the partially filled one, and `MONITOR_SYNC_MS` is 30 minutes
(`SolarTracker.h:59`), so in steady state a chunk never accumulates.

### `Telemetry::formatRow`

Extend rather than replace, same fixed order, still no heap surprises beyond the existing
`String`:

```cpp
String Telemetry::formatRow(uint32_t timestamp, float temperature, int extendedPercent,
                            const String &status, int windPeak,
                            bool settingsServer, bool windSensor, bool away,
                            const Diagnostics &diag)
{
  String row;
  row.reserve(96);              // was 48
  row += "v2,";
  // ... the existing eight fields, unchanged ...
  row += ','; row += String(diag.wedgeCount);
  row += ','; row += String(diag.recoverCount);
  row += ','; row += String(diag.lastI2cCode);
  row += ','; row += String(diag.freeRam);
  row += ','; row += diag.resetCause;   // 8 hex chars, formatted once at boot
  row += ','; row += diag.bootId;       // 4 hex chars, formatted once at boot
  row += ','; row += String(millis() / 1000UL);
  row += ','; row += FIRMWARE_VERSION;
  return row;
}
```

`test/test_telemetry.cpp` already asserts exact row strings at lines 23, 29 and 35 — those
expectations change with this PRD and are the cheapest proof the format is right.

### Reset cause

Verified on this machine, in the UNO R4 WiFi variant's own device header
`variants/UNOWIFIR4/includes/ra/fsp/src/bsp/cmsis/Device/RENESAS/Include/R7FA4M1AB.h`:
`RSTSR1` is a 16-bit register at line 9684, `RSTSR0` (8-bit) at 10202, `RSTSR2` (8-bit) at
10225. Bit positions at 16649-16667 and 16798-16810: `RSTSR1` carries `IWDTRF` b0,
`WDTRF` b1, `SWRF` b2, `RPERF` b8, `REERF` b9, `BUSSRF` b10, `BUSMRF` b11, `SPERF` b12,
`TZERF` b13, `CPERF` b15; `RSTSR0` carries `PORF` b0, `LVD0RF` b1, `LVD1RF` b2, `LVD2RF` b3,
`DPSRSTF` b7; `RSTSR2` carries `CWSF` b0 (cold/warm start).

```cpp
// src/Diagnostics/Diagnostics.cpp — called first thing in setup()
void Diagnostics::captureResetCause()
{
  uint16_t r1 = R_SYSTEM->RSTSR1;
  uint8_t  r0 = R_SYSTEM->RSTSR0;
  uint8_t  r2 = R_SYSTEM->RSTSR2;
  char buf[9];
  snprintf(buf, sizeof(buf), "%04X%02X%02X", r1, r0, r2);
  resetCause = buf;
  // Do not clear here on the first build. See open questions.
}
```

Reads need no unlock. The flags in `RSTSR1` are sticky, so leaving them uncleared makes the
field cumulative rather than per-boot; clearing them is a protected write that needs the
`R_SYSTEM->PRCR` PRC1 unlock/lock pair the core already uses in
`cores/arduino/boot.cpp:6-8`. Ship uncleared first, learn what the registers actually read
across a power cycle, the reset button and an ESP32-driven BOSSA reflash, then decide.
Sending the raw bytes is what makes that possible without a reflash.

`boot_id` is a nonce, not a counter: `(uint16_t)(micros() ^ (analogRead(A0) << 4) ^ analogRead(A1))`
using two pins the wiring doc lists as free (`docs/wiring.md`, header map, A0/A1 marked `-`).
Two boots collide with probability 1/65536, which is fine for telling adjacent sessions
apart. A monotonic counter in a 6-byte file on the card is the alternative if that ever
matters; it is not worth a new write path now.

### Versioning: three options

| Option | Mechanism | Verdict |
| --- | --- | --- |
| A. Width dispatch | server picks the parser from `parts.length` (8 vs 17) | **Rejected.** A v2 row truncated by a power cut mid-write can land on exactly 8 fields and parse as a valid v1 row with the wrong values in every column. Silent corruption is worse than a reject. |
| B. Leading `v2` tag | first field declares the schema; parser dispatches, unknown tag is a counted reject | **Recommended.** 3 bytes. A truncated v2 row still starts with `v2`, so it can never be mistaken for v1, and v3 needs no guesswork. |
| C. Device sends JSON | NDJSON straight from the board | **Rejected.** Roughly 3× the bytes for the same data, more RAM per request, and it throws away the truncation tolerance the CSV parser already has (`routes.test.js` "rejects a row truncated by a power cut mid-write"). |

### Server: `feature/telemetry-api`, `server/routes.js`

```js
const V1_FIELDS = ["time","temperature","extended_percent","status",
                   "wind_peak","settings","wind_sensor","away"];
const V2_FIELDS = ["schema", ...V1_FIELDS, "wedge_count","recover_count",
                   "last_i2c_code","free_ram","reset_cause","boot_id",
                   "uptime_s","firmware_version"];   // 17

// Returns { record } or { reject: "<reason>" }. Never null: the reason is the point.
export function parseRow(line) { ... }
```

Dispatch on `parts[0] === "v2"` first, fall through to the 8-column v1 path, and reject with
`schema` for any other leading token that is not a positive integer. The stored record stays
additive so anything already reading the `raw/` prefix keeps working:

```json
{"time":"2026-09-22T12:37:00.000Z","temperature":18.25,"extended_percent":35,
 "status":"ACTIVE","wind_peak":3,
 "button_panel":{"settings":false,"wind_sensor":false,"away":false},
 "schema":2,
 "diag":{"wedge_count":8,"recover_count":8,"last_i2c_code":2,"free_ram":21556,
         "reset_cause":{"raw":"00040101","flags":["RPERF","PORF","CWSF"]},
         "boot_id":"00a3","uptime_s":54321,"firmware_version":"1.4.0"}}
```

v1 rows get `"schema":1` and no `diag`. The `POST /data` handler gains reject accounting and
an archive of anything it could not store:

```js
const rejects = { width:0, schema:0, time:0, status:0, number:0, flags:0 };
// ... per line: rejects[reason]++ , and console.warn(JSON.stringify({
//   severity:"WARNING", event:"telemetry_reject", reason,
//   cols:parts.length, sample:line.slice(0,120) }))  -- first 5 per batch only
if (records.length === 0) {
  await storage.bucket(TELEMETRY_BUCKET)
    .file(`rejected/${day}/${stamp}.csv`).save(raw, { contentType:"text/csv" });
  console.warn(JSON.stringify({ severity:"ERROR", event:"telemetry_batch_rejected",
                                lines: lines.length, rejects }));
  return res.status(200).send({ saved: 0, skipped, rejects });
}
res.status(200).send({ saved: records.length, skipped, rejects, object: objectName });
```

Cloud Run already emits stdout/stderr JSON to Cloud Logging with `severity`, so a log-based
counter on `event="telemetry_reject"` plus an alert needs no new service and no new bill —
Cloud Logging is $0.50/GiB with the first 50 GiB/project/month free, and this project is
three orders of magnitude under that (unverified: the adversarial verification pass on the
research did not complete; pricing is from https://cloud.google.com/stackdriver/pricing and
https://docs.cloud.google.com/logging/docs/analyze/log-volume).

### Curl verification matrix, run before flashing anything

Against `https://test-image-188091506191.us-central1.run.app/data`, `Content-Type: text/csv`:

| Payload | Expected |
| --- | --- |
| v1 row `1788947478,22.25,29,ACTIVE,3,1,1,0` | `saved:1`, `schema:1`, no `diag` |
| v2 row, 17 columns | `saved:1`, `schema:2`, `diag` populated, `reset_cause.flags` decoded |
| v2 row truncated to 8 columns | `saved:0`, `rejects.width:1`, object under `rejected/` |
| 12-column junk | `saved:0`, `rejects.width:1`, archived |
| unknown status `SLEEPY` | `saved:0`, `rejects.status:1`, archived |
| decorated status `ACTIVE ` | `saved:0`, `rejects.status:1` — or trimmed and accepted, decide explicitly |
| empty body | `saved:0`, `skipped:0`, no archive, still 200 |
| mixed batch, 2 × v1 + 2 × v2 + 1 junk | `saved:4`, `skipped:1`, one object, one warn line |

### Firmware: reading the reply

`R4HttpClient::getBody()` exists (`R4HttpClient.h:140`) and `WifiClient::post()` currently
discards it before `http.close()`. Capture it into a member, then in `uploadLogs()`:

```cpp
int code = wifiClient.post(API + String("/data"), body, "text/csv");
if (code == 200) {
  StaticJsonDocument<192> reply;                 // ArduinoJson is already a dependency
  if (!deserializeJson(reply, wifiClient.lastBody())) {
    int saved = reply["saved"] | -1, skipped = reply["skipped"] | 0;
    if (saved <= 0 || skipped > 0)
      Serial.println("WARN upload stored " + String(saved) + " rows, " +
                     String(skipped) + " rejected");
  }
}
```

The chunk still clears on 200. Keeping it would stall the drain forever on one bad row,
which is what the `routes.js:111-113` comment already warns about; the server's `rejected/`
archive is what makes clearing safe.

### What this contradicts in the project's own docs

- **`SPEC.md` is wrong about the sink.** It documents `server/` as "Node 20 / Express /
  Mongoose" (`SPEC.md:32`) writing a telemetry document to MongoDB (`SPEC.md:173,178`), and
  says at `SPEC.md:282` that "Telemetry is not wired up. `WifiClient::post()` is an empty
  `// TODO`". Telemetry is wired up, it POSTs CSV, and the deployed service writes NDJSON to
  Cloud Storage. FR-34 has to be rewritten, not amended.
- **"Silently drops with HTTP 200" is not quite the failure.** The server does log
  `Rejected every row of a ${lines.length} row batch` (`routes.js:126`) and every reply
  carries `skipped`. The silence is entirely on the device: no log in this repo contains the
  string `saved` — `grep -c saved bench/logs/*.log` returns 0 everywhere, while
  `bench/logs/14_monitor_build-20260921T095600.log:13-14` shows
  `Response code: 200` / `Uploaded 1 chunk(s)` with the body never printed. The visibility
  fix therefore has a device half, and it is one line plus a member.
- **The research's telemetry byte estimate is 5× high.** The reliability dimension budgets
  144 rows/day × 170 B = 24.5 KB/day. The card says 33.4 B/row. Even the widened v2 row is
  well under the figure the research was already calling negligible.
- **The wind_peak hack is costing wind data right now, not hypothetically.**
  `bench/logs/14_monitor_build-20260921T095600.log:22` prints `Wind Sensor: 0`, so the
  selector is in the wedge position and `wind_peak` has been carrying wedge counts, not
  wind, since that build was flashed.

## Risks

| Risk | Likelihood | Impact | Mitigation |
| --- | --- | --- | --- |
| Firmware flashed before the server accepts v2; every row rejected and deleted from the card | Medium | High — silent total telemetry loss | Server first, curl matrix green before the board is touched. The order of work above is not advisory. |
| A bug in the new row formatting bricks or hangs the sketch, forcing a site visit | Low | High — a visit is the dominant project cost | No new blocking calls, no network calls, no I2C reads added. Everything new is a register read at boot, a `sbrk` subtraction and string appends. Bench-run the exact binary first. |
| `reset_cause` registers read as zero or as stale sticky bits, making the field useless | Medium | Low | Raw hex means the decode is fixed server-side without a flash. Bench-verify across power cycle, reset button and reflash before drawing conclusions. |
| Row growth halves outage tolerance from ~50 to ~24 days | High (arithmetic, certain) | Medium — a long outage loses the oldest rows | Raise `maxChunks` to 128 (requirement 18). 128 × 4 KB = 512 KB on a 32 GB card, and `tlm127.csv` still fits 8.3. |
| `maxChunks` change interacts with `selectWriteIndex()`'s resume scan and re-reads 128 files at boot | Medium | Low | The scan is `chunkSize()` per index, 128 `SD.open` calls once per boot. Measure it in the bench run; if it is slow, scan downward and stop at the first hit (it already does). |
| Flash or RAM regression | Low | Medium | Requirement 17 is a hard gate measured with `arduino-cli compile`; the baseline is 127,688 B / 11,212 B, 48% of flash. |
| Reject archive to `rejected/` grows unbounded if a schema mismatch persists | Low | Low | Bucket lifecycle rule, 30-day delete on the `rejected/` prefix. Costs nothing at these sizes. |
| v1 acceptance never gets removed and the parser carries two schemas forever | Medium | Low | The response now counts v1 rows; removal is gated on a week of zeros, which is observable rather than remembered. |

## Acceptance criteria

- [ ] `curl` against the deployed service returns `saved:1, schema:1` for an 8-column v1 row
      and `saved:1, schema:2` for a 17-column v2 row.
- [ ] A 12-column payload, an unknown-status payload and a v2 row truncated to 8 columns all
      return `saved:0` with the correct `rejects` key, and each one appears as an object
      under `gs://solar-tracker-442903-telemetry/rejected/`.
- [ ] A Cloud Logging query for `jsonPayload.event="telemetry_reject"` returns one entry per
      rejected row class from the matrix above, at `severity>=WARNING`.
- [ ] `server/routes.test.js` passes with cases for each reject reason and for a mixed
      v1/v2 batch; `npm test` is green.
- [ ] `test/test_telemetry.cpp` asserts the exact v2 row string, including the `v2` tag and
      field order, and the native suite passes.
- [ ] `arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi solar-tracker.ino` reports
      under 135,000 bytes of flash and globals within 64 bytes of 11,212.
- [ ] A bench run with `BENCH_TEST` fills and uploads at least three chunks, and the serial
      log shows no `WARN upload stored` line.
- [ ] Objects under `raw/<today>/` contain `diag.free_ram` within a few hundred bytes of the
      `ram=` value the bench heartbeat prints in the same minute.
- [ ] Three deliberate resets — power cycle, reset button, `NVIC_SystemReset()` from a bench
      sketch — produce three distinguishable `reset_cause` values, recorded in
      `docs/diagnostics/`.
- [ ] `boot_id` differs across two consecutive power cycles, and `uptime_s` increases
      monotonically within one `boot_id` even across a row whose `time` is 946684800.
- [ ] A row stamped during an induced wedge shows `last_i2c_code` 2 or 5 and a
      `wedge_count` that increments in the cloud rows — the same recovery the field log on
      `home` recorded 8 times at 08:37 on 2026-09-22.
- [ ] `SPEC.md` FR-34 describes the CSV row and the Cloud Storage NDJSON object, with no
      mention of MongoDB.

## Open questions

- Which build is actually on the tracker right now? Memory says the SD branch is what runs,
  but `sd-support` has no `src/Telemetry` at all, while
  `bench/logs/14_monitor_build-20260921T095600.log` shows a `feature/telemetry` build POSTing
  to the live Cloud Run URL. This decides whether the v1 rows still on the card carry wedge
  counts in `wind_peak` or plain wind, and therefore how the transition window is read.
- Do the `RSTSR1` flags survive the ESP32-driven BOSSA reset that PRD 02 will use to reflash
  the RA4M1, and do they survive the double-tap bootloader entry
  (`cores/arduino/boot.cpp`)? If a reflash looks identical to a watchdog reset, the field
  misleads exactly when it matters.
- Should the sticky reset flags be cleared after reading? Uncleared, the field is cumulative
  across the life of the board; cleared, it is per-boot but needs a protected-register write.
  Decide from one bench session, not from the datasheet.
- What is the canonical firmware version string, and who bumps it? There is no version
  constant anywhere in the tree today and no `build_opt.h`, so this is a new hand-maintained
  file unless the build gains `git describe` injection. PRD 02 cannot start until the token
  is decided.
- Is `FIRMWARE_VERSION` in the row the only version the tracker should report, or does PRD 02
  also need `WiFi.firmwareVersion()` for the ESP32 bridge? The bridge version gates OTA at
  0.4.1, every boot prints `Please upgrade the firmware` (29 times in one 2 h capture:
  `docs/diagnostics/serial-2026-09-10.log`), and that warning only proves the bridge is below
  0.6.0 — not that OTA is unavailable (unverified: research claim from per-tag source
  inspection, https://github.com/arduino/uno-r4-wifi-usb-bridge; the verification pass did
  not complete). One extra column now is cheaper than a second schema bump later.
- Should a decorated status (`"ACTIVE "`, trailing whitespace) be trimmed and accepted or
  rejected? Rejecting is stricter; trimming is closer to "never drop". Pick one and write
  the test.
- Every research claim used here is well-sourced but unverified, because the adversarial
  verification pass did not complete. The two load-bearing numbers this PRD rests on were
  re-checked directly in the repo and do hold: 4,072 B / 122 rows = 33.4 B per row
  (`bench/logs/00_card_inventory-20260918T114838.log:19`) and zero occurrences of `saved` in
  any captured log.
