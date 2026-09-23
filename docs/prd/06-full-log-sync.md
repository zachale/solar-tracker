# 06 — Ship the full diagnostic log to the cloud

**Status:** Proposed
**Owner:** unassigned
**Depends on:** nothing. Deliverable standalone. Soft sequencing notes are in Open questions.
**Blocks:** nothing, but every other diagnostic PRD is cheaper to verify once this exists.
**Estimate:** 5–6 working days (device 3–4, server/GCP 1–1.5, bench soak elapsed on top)
**Cost:** $0.00/month. Arithmetic in Design → "Sink choice".

## Problem

On 2026-09-22 at 08:37 the board printed `I2C bus unhealthy (code 2)` and
`Bus clear: sda_freed=1 recovered=1` eight times and recovered every time, and none of
that text left the device — the only thing that reached the cloud was the integer `8`
smuggled into the `wind_peak` column (`SolarTracker.cpp:155-161`). The cloud can say
*that* something happened and never *what*. `docs/diagnostics/README.md` states the
situation plainly: "Serial output is not recorded anywhere by the device itself. If it is
not captured while a laptop is attached, it is gone." The one reason this is survivable
today is that the tracker is plugged into Zach's home PC, which is running
`docs/diagnostics/capture-serial.sh` in its own checkout — a USB cable is currently load
bearing infrastructure for a remote site.

## Goal

Every diagnostic line the firmware emits is queryable in Cloud Logging within one sync
interval, correctly ordered even when the RTC is wedged, with no USB cable attached.

## Non-goals

- Does not replace or modify the home PC capture. That setup stays the source of truth
  until this ships and is proven against it, and is the reconciliation reference during
  the build (see Acceptance criteria).
- Does not fix `ClockModule::isHealthy()`, the per-loop I2C probe rate, or the
  `wind_peak` field smuggling. This PRD makes those faults *visible*; a separate PRD
  fixes them. It must land without changing tracking, actuator, or telemetry behaviour.
- Does not add a watchdog, MQTT, or any persistent connection. Logs ride the existing
  hourly `dailySync()` window.
- Does not change the `POST /data` telemetry contract or its 8-column schema.
- Does not attempt on-device alerting, log search, or a log view in the settings
  dashboard.
- Does not build a BigQuery pipeline for logs. Cloud Logging plus a Log Router archive is
  the whole storage design.

## Requirements

1. Replace all 105 `Serial.print*` call sites with leveled macros. Verified in this tree:
   `WifiClient.cpp` 17, `SolarTracker.cpp` 28, `SettingsServer.cpp` 23,
   `LinearActuator.cpp` 13, `Clock.cpp` 10, `SettingsManager.cpp` 4, `solar-tracker.ino`
   10 — six modules plus the sketch.
2. Format every log line into a fixed `char` buffer with `snprintf` and use no Arduino
   `String` anywhere in the logging path, because sites like `SolarTracker.cpp:162`
   allocate and free several `String`s per call and there are only 21,556 bytes of
   locals+stack to fragment.
3. Buffer log lines in RAM and flush to the card in blocks, never once per line
   (unverified: the SD wear model behind this is the research pass's own arithmetic at
   *medium* confidence, and the card's real erase-block size is unknown — see Open
   questions).
4. Demote the seven-line 10-second poll block (`SolarTracker.cpp:70-92`) to DEBUG,
   compile it out of field builds, and collapse it to a single line even at DEBUG, since
   it is 79.2% of all log volume and restates the telemetry row already shipped.
5. Collapse repeated identical lines into a counted single entry — measured in one 1.96 h
   capture: `Please upgrade the firmware` ×29, `Response code: 200` ×64.
6. Stamp every line with a boot id and a monotonic `millis()` sequence number alongside
   the wall clock, so ordering survives an RTC fault that prints `0:0:0` (×64 in that
   same capture) or stamps the 2000-01-01 epoch.
7. Reconstruct wall-clock timestamps server-side from the batch's arrival time and the
   per-line `millis()` delta, so entries written during a wedge do not scatter to the
   year 2000 and fall outside the 30-day retention window.
8. Add a second Cloud Run endpoint `POST /logs` that parses the device's line format and
   writes to Cloud Logging under the service's own identity with
   `roles/logging.logWriter` — the RA4M1 cannot write to Cloud Logging directly
   (unverified: rests on the research claim that `entries:write` has no API-key path,
   only OAuth scopes).
9. Return the count of rejected lines in the `/logs` response body and have the firmware
   read it, so a firmware/server format skew is visible rather than silently swallowed
   the way `POST /data` swallows a bad row today (`server/routes.js:113,127` answer 200
   with a `skipped` count that `WifiClient::post` throws away).
10. Redact WiFi credentials on the device before any line reaches the card. Two call
    sites print them today: `SettingsServer.cpp:200` serializes every submitted dashboard
    parameter, and `solar-tracker.ino:42` prints the whole config doc. `capture-serial.sh`
    redacts these host-side; nothing does on the device.
11. Never flush the log buffer from inside the actuator drive loop. `loop()` is re-entered
    with the motor energised, and an SD write there delays stall detection, which is the
    end-stop detector.
12. Upload telemetry chunks before log chunks in every sync, and never let a log-upload
    failure abort or delay a telemetry upload.
13. Size the on-card log ring for at least 7 days of field-verbosity backlog, because the
    settings switch puts the radio in AP mode and nothing reaches the internet while it
    is on, so logs can sit on the card for an operator's entire visit and arrive in a
    burst.
14. Route Cloud Logging to a GCS archive under a prefix that is **not** `raw/`, because
    `server/routes.js:132` documents that BigQuery reads the whole `raw/` prefix as one
    external table and log objects there would break it.
15. Emit a single measurable free-RAM and flash number in the boot log, and gate the flash
    of this work on both staying inside the budget in Acceptance criteria.

## Design

### Do this in order

1. Write `src/Logger/` and a level enum. No call-site changes yet. Measure flash and RAM
   delta on an otherwise unmodified sketch.
2. Convert the 105 call sites mechanically, one commit, no behaviour change. Diff the
   bench serial capture before and after: it must be line-for-line equivalent at DEBUG.
3. Add the on-card log ring and the RAM buffer. Run the existing `bench/03_sd_stress`
   and `bench/10_overnight_soak` harnesses.
4. Stand up `POST /logs` on the `feature/telemetry-api` branch (the deployed one) and
   test it with `curl` against a captured batch before the firmware ever posts.
5. Wire the firmware's log upload into `uploadLogs()`, after the telemetry loop.
6. Add the log-based metrics and alert policy. Only then stop relying on the home PC
   capture.

### Device-side logger

```cpp
// src/Logger/Logger.h
enum LogLevel : uint8_t { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 };

#if BENCH_TEST
  #define LOG_COMPILED_LEVEL LOG_DEBUG
#else
  #define LOG_COMPILED_LEVEL LOG_INFO
#endif

#define LOG_E(mod, fmt, ...) Logger::emit(LOG_ERROR, mod, F(fmt), ##__VA_ARGS__)
#define LOG_W(mod, fmt, ...) Logger::emit(LOG_WARN,  mod, F(fmt), ##__VA_ARGS__)
#define LOG_I(mod, fmt, ...) Logger::emit(LOG_INFO,  mod, F(fmt), ##__VA_ARGS__)
#if LOG_COMPILED_LEVEL >= LOG_DEBUG
  #define LOG_D(mod, fmt, ...) Logger::emit(LOG_DEBUG, mod, F(fmt), ##__VA_ARGS__)
#else
  #define LOG_D(mod, fmt, ...) do {} while (0)
#endif
```

`Logger::emit` does one `vsnprintf` into a single `static char line[128]`, writes it to
`Serial` unchanged (so the home PC capture keeps working identically), and appends it to
a `static char ring[512]` staging buffer. `LOG_D` compiling to `do {} while (0)` is what
makes requirement 4 free in field builds — the format strings do not even reach flash.

Call-site conversion, using the line this PRD exists for:

```cpp
// before — SolarTracker.cpp:40
Serial.println("I2C bus unhealthy (code " + String(ClockModule::lastI2cCode) +
               "), attempting bus clear #" + String(ClockModule::wedgeCount));
// after
LOG_E("CLK", "i2c_unhealthy code=%u wedge=%u",
      ClockModule::lastI2cCode, ClockModule::wedgeCount);
```

A stable leading token (`i2c_unhealthy`, `bus_clear`, `sync_fail`) is what the log-based
metrics match on, so it is part of the message contract, not cosmetic.

### Wire format

One line per entry, pipe-delimited because telemetry already owns comma-CSV and log
messages contain commas. Six fields then a free-form message:

```
#v1 boot=41 fw=71b63aa post_ms=85412100
E|41|10237|85321004|1789042620|CLK|i2c_unhealthy code=2 wedge=8
E|41|10238|85321061|1789042620|CLK|bus_clear sda_freed=1 recovered=1 total_wedges=8 total_recovered=8
I|41|10239|85402330|0|NET|sync_ok chunks=3 bytes=11208
```

`lvl | boot | seq | millis | unix_or_0 | module | message`. The server splits on the
first six `|` so the message may contain anything. Lines are capped at 120 bytes, which
is what keeps the 128-byte format buffer sufficient.

`boot` is a counter in a new one-line file `bootid.txt` on the card, incremented once in
`setup()`. Nothing existing is touched — no card wipe, no RTC write. If the card is
absent the boot id is `0` and the server treats it as un-orderable, which is correct
because without a card there is no ring either.

### Timestamp reconciliation

The batch header carries `post_ms`, the `millis()` value at POST time. The server knows
its own receive time `T`. For any line with `unix=0` or an out-of-range wall clock:

```
timestamp = T - (post_ms - line_millis) milliseconds
```

Entries then land within one round-trip of their true time even with a dead RTC, and
`(boot, seq)` breaks ties and proves ordering. `insertId` is set to `<boot>-<seq>` so a
retried batch does not duplicate (unverified: Cloud Logging's `insertId` dedupe semantics
were not covered by the research pass — confirm before relying on it, and make the
firmware's retry path idempotent regardless).

### On-card ring

`log0.txt` … `log31.txt`, 4,096 bytes each, 128 KB total — same numbered-chunk,
delete-after-200 shape as `Telemetry`, in a separate filename namespace so the two rings
cannot interfere. At the recommended field verbosity of ~12.8 KB/day that is ~10 days of
backlog, satisfying requirement 13. At full DEBUG (1.27 MB/day) it is 2.4 hours, which is
fine because DEBUG is a bench build with a USB cable attached.

**Correction to the research, and it is load bearing.** The research claims "the correct
buffering pattern already exists in this codebase" and that `Telemetry.cpp` "buffers 4 KB
before writing". It does not. `Telemetry::append` (`src/Telemetry/Telemetry.cpp:107-123`)
does `SD.open` → `print` → `close` on **every single row**. The 4,096 bytes in
`Telemetry.h:37` is a *file-size ceiling*, not a RAM buffer. That is harmless at 144
rows/day and is exactly the write-amplification hazard the research warns about at 53,175
lines/day. There is no pattern to generalise: the 512-byte RAM staging buffer is new work,
and it is the thing requirement 3 is actually asking for.

Flush triggers: buffer full; 60-second timer; or an ERROR line, rate-limited to one
forced flush per 5 s so a wedge storm cannot turn into a write storm. Every trigger is
suppressed while `actuator.status != LinearActuator::STOP` (requirement 11); if the
buffer overflows during a move, the oldest DEBUG lines are dropped and a single
`W|...|LOG|dropped n=<count>` line records it. Dropping DEBUG is always preferable to
holding the motor on.

Write amplification, using the research's erase-block model:

| Buffer | Flushes/day at 1.27 MB/day | Metadata writes/day |
| --- | --- | --- |
| none (per line) | 53,175 | 106,350 |
| 512 B | 2,476 | 4,952 |
| 4,096 B | 310 | 620 |

512 bytes is the recommendation. It is a 21× reduction on the unbuffered case, and at
realistic field verbosity (12.8 KB/day) it is ~50 metadata writes/day — noise. 4,096
bytes is only 8× better than that and costs 4 KB of the 21,556 bytes of locals+stack,
which already has to hold a 4 KB telemetry chunk `String` plus the POST body copy during
a sync. The RAM is worth more than the wear margin.

### Sink choice

Cost is not a discriminator, so this is decided on queryability and alerting.

| Sink | Cost/month at 38.6 MB | Query | Alerting | Verdict |
| --- | --- | --- | --- | --- |
| **Cloud Logging** (+ Log Router to GCS) | **$0.00** (0.754 GiB vs 50 GiB free) | `gcloud logging read`, Log Analytics SQL | log-based metrics + alert policies, native | **Recommended** |
| GCS object per batch | ~$0.005 | none without a job | none | Rejected: identical to what telemetry already does, and the whole complaint is that it is not queryable |
| BigQuery (gRPC Storage Write) | $0.00 | SQL | needs a scheduled query | Rejected: more moving parts than a 0.6 entries/second device justifies |

Arithmetic, verified against this repo's own capture
(`docs/diagnostics/serial-2026-09-10.log`): 4,334 lines over 7,042 s (18:26:48 →
20:24:10); 198,660 total bytes − 4,334 × 21-byte host prefix − 4,334 newlines = 103,312
board-emitted bytes = 23.8 B/line. Projected: 4,334 ÷ 7,042 × 86,400 = **53,175
lines/day** × 23.8 B = 1.27 MB/day × 30.44 = **38.6 MB/month**.

Cloud Logging bills `storage_bytes` (payload plus serialization and metadata), not raw
payload, so the pessimistic case is one entry per line at 500 B billed:
53,175 × 500 = 26.6 MB/day = 809 MB/month = **0.754 GiB**, against a free allotment of
**50 GiB/project/month** at $0.50/GiB beyond it
([pricing](https://cloud.google.com/stackdriver/pricing),
[billable volume](https://docs.cloud.google.com/logging/docs/analyze/log-volume)) —
**66× headroom, $0.00**. Breakeven is ~41 sustained entries/second; this board emits 0.6.
The recommended leveled build (~356 lines/day) is 0.39 MB/month, 1,150× under.

Archive: Log Router sink → `gs://solar-tracker-442903-telemetry/logs/`. 38.6 MB ×
$0.020/GB = **$0.00077/month**, and log exports to Cloud Storage are written with
internal APIs so they incur no Class A operation charges (`Log Router — no additional
charge`, same pricing page). The `logs/` prefix, not `raw/`, per requirement 14.

Cloud Run: +24 requests/day = 730/month against 2,000,000 free → $0.00.

**Total: $0.00/month.** At 66× headroom on the single largest line item, this rounds to
zero even if every estimate above is wrong by an order of magnitude.

### Server endpoint

On `feature/telemetry-api` (the deployed branch; `main` is the dead MongoDB version):

```js
app.use(express.text({ type: "text/plain", limit: "256kb" }));

app.post("/logs", async (req, res) => {
  const { entries, dropped } = parseLogBatch(req.body); // header + 6-field lines
  const log = logging.log("solar-tracker");
  await log.write(entries.map(e => log.entry({
    resource: { type: "generic_node", labels: {
      project_id: PROJECT, location: "us-central1",
      namespace: "solar-tracker", node_id: `boot-${e.boot}` } },
    severity: e.severity,              // ERROR | WARNING | INFO | DEBUG
    timestamp: e.timestamp,            // reconciled, see above
    insertId: `${e.boot}-${e.seq}`,
    labels: { boot_id: String(e.boot), seq: String(e.seq), module: e.module },
  }, { message: e.message, device_millis: e.millis, rtc_unix: e.unix })));

  if (dropped > 0) console.warn(`/logs dropped ${dropped} unparseable lines`);
  res.status(200).send({ accepted: entries.length, dropped });
});
```

Needs `@google-cloud/logging` added to `server/package.json` (which today carries only
`@google-cloud/storage` and `express`) and `roles/logging.logWriter` on the Cloud Run
service account.

The device must read that `dropped` count, which `WifiClient::post` cannot do — it
returns `responseNum` and discards the body (`WifiClient.cpp:122-125`). Add an optional
out-parameter:

```cpp
int post(String url, const String &body, const char *contentType, String *responseOut = nullptr);
```

and have `uploadLogs()` log `W|...|NET|logs_dropped n=<dropped>` when it is non-zero.
That one line is the difference between this endpoint and `POST /data`, which answers 200
and silently drops anything that is not exactly 8 CSV columns.

### Alerting

Two log-based counter metrics on `logName="…/solar-tracker"`:
`jsonPayload.message:"i2c_unhealthy"` and `jsonPayload.message:"sync_fail"`, each with an
alert policy. **Absence detection does not belong here.** Logs can be hours late by
design — AP mode means nothing reaches the internet while the settings switch is on — so
"no logs" must never be the alert condition. A "no telemetry row in 3 hours" alert on the
existing `/data` path is the liveness check; this PRD's alerts are for things that *did*
happen.

## Risks

| Risk | Likelihood | Impact | Mitigation |
| --- | --- | --- | --- |
| Converting 105 call sites across 6 modules regresses behaviour in the field build → **makes a site visit more likely** | Medium | High | Conversion lands as one mechanical commit with no logic change; DEBUG-build serial output diffed line-for-line against the pre-change bench capture; full `bench/` matrix plus a 24 h soak before flashing |
| Log flush inside the actuator drive loop delays stall detection and drives the panel past its target → **site visit** | Medium | High | Requirement 11: flush gated on `actuator.status == STOP`; overflow drops DEBUG lines instead of blocking; the drive-loop iteration rate is measured on the bench before this ships |
| RAM fragmentation or exhaustion from the new buffers plus any surviving `String` use → hang with no watchdog → **site visit** | Medium | High | Logger path is `snprintf`-only; 512 B not 4,096 B buffer; free RAM printed at boot and asserted in Acceptance criteria; soak measures drift, not just a start value |
| WiFi passwords ship to Cloud Logging from `SettingsServer.cpp:200` / `solar-tracker.ino:42` | High if unaddressed | High | Requirement 10: on-device redaction, and those two lines demoted to DEBUG so they are compiled out of field builds entirely |
| SD ring wrap loses the lines describing the fault, because the `/logs` endpoint is broken and the device keeps spooling | Medium | Medium | Ring sized for ~10 days at field verbosity; `/logs` tested with `curl` against a captured batch before firmware posts to it; telemetry upload never blocked on log upload (requirement 12) |
| Timestamps scatter to 2000-01-01 and age out of the 30-day window | High without reconciliation | Medium | Requirements 6–7; `(boot, seq)` is the ordering key and the wall clock is advisory |
| Bench-only verification misses a field-only path, and fixing it needs a physical trip because OTA is not available | Medium | High | Note in Open questions: the bridge firmware gates OTA, and every iteration of this work is a site visit until it is upgraded over USB |
| A wedge storm turns the immediate-ERROR-flush into an SD write storm | Low | Medium | Forced flush rate-limited to 1 per 5 s |

## Acceptance criteria

- [ ] `gcloud logging read 'logName="projects/solar-tracker-442903/logs/solar-tracker" AND severity>=WARNING'` returns a bench-forced I2C wedge as two entries — `i2c_unhealthy code=… wedge=…` and `bus_clear sda_freed=… recovered=…` — with `labels.boot_id` and `labels.seq` populated.
- [ ] Those entries are in correct `seq` order, and carry a timestamp within 60 s of the host capture's own timestamp for the same line, **while the RTC is reading `0:0:0`** (reproduce by pulling the RTC power pin D8 low mid-run).
- [ ] Every line in a 2-hour bench run appears exactly once in Cloud Logging and once in the home PC's `capture-serial.sh` output, reconciled by `(boot, seq)`. Line counts match; no duplicates, no gaps.
- [ ] `POST /logs` with a batch produced by the matching firmware returns `dropped: 0`; a deliberately corrupted line returns `dropped: 1` and produces a server-side `WARNING` entry.
- [ ] Field-build volume measured over 24 h on the bench is under 1,000 lines/day (target ~356), confirming the DEBUG poll block is compiled out.
- [ ] Field build flash ≤ 140,000 bytes of 262,144 (from 127,688) and global RAM ≤ 12,200 bytes of 32,768 (from 11,212), read off `arduino-cli compile` output.
- [ ] Free RAM printed at boot and after a 24 h soak differs by less than 500 bytes, from a baseline of 21,556 bytes of locals+stack.
- [ ] The logger's flush counter over a 24 h field-verbosity bench run shows ≤ 100 SD flushes/day.
- [ ] A WiFi password submitted through the settings dashboard appears nowhere in the on-card ring, the `/logs` request body, or Cloud Logging — checked by grepping all three for the exact string.
- [ ] A commanded actuator move completes to within its normal step tolerance with the logger active at DEBUG, with no new `boundary hit` or recalibration lines versus the pre-change bench run.
- [ ] Telemetry rows continue to arrive in `gs://solar-tracker-442903-telemetry/raw/` unchanged in schema and cadence throughout the soak, and log archive objects appear only under `logs/`.
- [ ] An alert policy on the `i2c_unhealthy` log-based metric fires within 10 minutes of a bench-forced wedge reaching the cloud.

## Open questions

- **The verification pass did not complete.** Every number in this document traces to the
  research pass or to a file in this tree, but none of it was adversarially checked. The
  load-bearing ones are: the 53,175 lines/day and 79.2% figures (re-derived here from
  `docs/diagnostics/serial-2026-09-10.log` and confirmed to the byte, so these are the
  safest); the 50 GiB Cloud Logging free allotment; the absence of an API-key path for
  `entries:write`; and the SD wear model, which the research itself rated only *medium*
  confidence.
- **The 2026-09-22 08:37 capture is not in this checkout.** The eight wedges are on the
  home PC. Pull that capture into `docs/diagnostics/` before building, because it is the
  exact batch the `/logs` parser should be tested against, and it will age out of the
  home PC's disk the same way Cloud Logging's 30-day retention aged out the August
  evidence.
- **What erase-block size does the fitted SanDisk 32 GB High Endurance card actually
  use?** The wear arithmetic swings ~40× between a 128 KB and a 4 MB block. It does not
  change the 512-byte-buffer recommendation, but it changes whether the unbuffered case
  is "bad" or "fatal".
- **Is `insertId` dedupe reliable enough to make retried log batches idempotent**, or does
  the firmware need to track which log chunks it has already had accepted? Not covered by
  the research.
- **`generic_node` vs `generic_device` as the monitored resource type.** `generic_node` is
  the safe choice; confirm whether `generic_device` exists and reads better in the console
  before committing, since changing it later splits the log stream.
- **Two research findings contradict each other on the OTA blocker.** The logging research
  lists "cannot be delivered over the air — the sketch is already 4,808 bytes past the
  122,880-byte `InternalStorage` ceiling" as a blocker. The OTA research confirms in source
  that the ESP32 `+OTAUPDATE` path stages into the ESP32's own 512 KB SPIFFS, so that
  ceiling does not apply and a ~100 KB `.ota` has roughly 5× headroom. The flash ceiling is
  **not** the blocker; the sub-0.4.1 bridge firmware is, and it can only be updated over
  USB. Until that USB trip happens, every iteration of this work is a site visit — which is
  the argument for doing the bridge upgrade on the same trip as the first flash of this
  logger.
- **How long does the home PC capture stay the control?** Proposal: keep both running for
  one full week after the alert policy fires correctly, then stop treating the USB capture
  as required — but never remove `capture-serial.sh`, because it is the only thing that
  works when the radio is in AP mode.
- **Does the `wind_peak` smuggling get retired in the same release?** Once wedge counts
  arrive as real log entries, the `ButtonPanel::isWindSensorEnabled()` selector is a
  liability rather than a feature: if D4 floats HIGH the field silently reverts to wind
  data. That removal belongs to another PRD, but it should not ship *before* this one.
