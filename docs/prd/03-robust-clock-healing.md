# 03 — Harden clock health detection and recovery

**Status:** Proposed
**Owner:** unassigned
**Depends on:** nothing
**Blocks:** nothing
**Estimate:** 6–9 h of build and bench work (2–3 h in `Clock`, 1 h in `SolarTracker`, 1 h fault-injection sketch, 2–4 h bench validation), plus one unattended overnight run. Two calendar days.
**Cost:** none. Board-side only — no new cloud requests, no new columns, no change to the 144 rows/day (~24.5 KB/day) already POSTed.

## Problem

`ClockModule::isHealthy()` is two tests — does `0x68` ACK, and is the year inside 2020..2099 (`src/Clock/Clock.cpp:15-25`) — so the garbage reading observed in the field on 2026-09-22, `2043-04-29T18:05:44`, passes both, and the address ACK is the only thing between a junk timestamp and a telemetry row written as truth. Nothing downstream catches it either: `parseRow()` on the deployed branch only requires `Number.isInteger(seconds) && seconds > 0` (`server/routes.js:45-55`, `origin/feature/telemetry-api`), so a year-2043 row is stored in `gs://solar-tracker-442903-telemetry/raw/` as fact. This has already happened at lower years — three whole chunks on the card, 360 rows, carry `first_unix == last_unix == 946776240` (2000-01-02T01:24:00Z), a frozen clock spooled for hours, and all of them were uploaded on 2026-09-18 (`bench/logs/00_card_inventory-20260918T114838.log`; `bench/README.md` incidental finding 4). Meanwhile the same function runs from `pollSensorData()` on **every** `loop()` iteration (`src/SolarTracker/SolarTracker.cpp:37`, reached from `solar-tracker.ino:94`), costing roughly 1 ms of bus time per call — a 9-bit address frame plus `rtc.now()`'s seven-register burst at the core's default 100 kHz — for a decision needed once per 10-minute poll.

## Goal

A garbage RTC reading is rejected before it can reach a telemetry row, and the health check costs one I²C transaction per minute instead of one per `loop()` iteration.

## Non-goals

- Does **not** find the wedge root cause. Motor current is still the only untested hypothesis (`bench/README.md`, "What is left").
- Does **not** add a watchdog, a `NVIC_SystemReset` path, or any bridge-hang recovery. Research flags the absent WDT as a separate, larger piece of work.
- Does **not** change how wedge counts reach the cloud. They stay smuggled in `wind_peak` (`src/SolarTracker/SolarTracker.cpp:154-156`); no new columns, no server change, no redeploy.
- Does **not** modify `busClear()`'s 9-clock sequence. It works — see the acceptance evidence below.
- Does **not** write to the RTC, the SD card, `wifi.jsn` or `config`. No `rtc.adjust()`, no file deletion, nothing that could wipe the card.
- Does **not** change the actuator schedule, the extension maths, or any motion behaviour.

## Requirements

1. Take exactly **one** `DateTime` snapshot per health evaluation and derive year, hour, minute, second and unixtime from that one snapshot, rather than calling `rtc.now()` separately in `getHour()`, `getMinute()`, `getSecond()` and `getTimestamp()` (`src/Clock/Clock.cpp:191-204`, `289-292`).
2. Reject a reading whose `unixtime()` is **less than** the last good value, or **equal** to it when at least 2 s of `millis()` has elapsed since that value was taken (this is the frozen-clock case, which is what the card actually holds).
3. Reject a reading more than `(millis() elapsed in seconds) + 1800` **ahead** of the last good value.
4. Reject a reading whose `getTemperature()` returns exactly 17.00 (`> 16.999f && < 17.001f`). *(unverified: the "register `0x11` left in RTClib's buffer" explanation is the project's own bench-harness comment at `bench/common/bench_common.h:50` and `bench/README.md`; the RTClib/Adafruit_BusIO `write_then_read()` failure path was not re-read against library source in this round. The empirical correlation is unaffected — 64 of 64 bad blocks in `docs/diagnostics/serial-2026-09-10.log` carried it.)*
5. Keep the raw `Wire.endTransmission()` probe to `0x68` as the first test and tighten the year window to `2026..2099`.
6. Evaluate at most once per `HEALTH_CHECK_MS` (60,000) and return the cached verdict otherwise, gated on `millis()` and never on the RTC.
7. Force a fresh evaluation immediately before `spoolTelemetryRow()` and immediately after `busClear()`, via an explicit `isHealthy(true)`.
8. Clear the last-good reference in `setDateTime()` and `setSimpleTime()` so a legitimate clock correction is never read as a backwards jump.
9. Stamp the telemetry row with `lastGoodUnix + elapsed` when the current reading is rejected, and never write that estimate back to the RTC.
10. Keep the verdict advisory: on unhealthy the only permitted actions are log, increment counters, and attempt a bus clear. It must never gate actuator motion, spooling, or syncing.
11. Defer a bus clear while `actuator.status != LinearActuator::STOP`, mirroring the gate `serviceSync()` already uses (`src/SolarTracker/SolarTracker.cpp:134-146`).
12. Back the interval off to 10 min after 3 consecutive clears that did not recover, and emit **one** line per clear instead of today's two.
13. Emit one machine-parsable line per rejection carrying the fault code, in the `key=value` shape the bench logs already use.
14. Report measured flash and RAM after the change against the 127,688 B / 11,212 B baseline before anything is flashed to the field unit.

## Design

### Which test catches which observed garbage

Every garbage value in the project's own records, against the current tests and the proposed ones:

| Observed reading | Where | Addr ACK | Year window (today) | Monotonic / frozen | Forward bound | Temp == 17.00 |
| --- | --- | --- | --- | --- | --- | --- |
| `2043-04-29T18:05:44` (epoch 2313943544) | field, 2026-09-22 | passes | **passes** | passes (it is later) | **catches**: +523,877,324 s ≈ 16.6 y vs. an allowance of elapsed + 1800 s | catches only if the temp read failed in the same transaction |
| `1478571780` = 2016-11-08T02:23:00Z, frozen across 47 blocks | `docs/diagnostics/serial-2026-09-10.log` | passes | catches | catches | — | catches |
| `1495506120` = 2017-05-23T02:22:00Z, frozen across 17 blocks | same capture, same 50-minute window | passes | catches | catches | — | catches |
| `946776240` = 2000-01-02T01:24:00Z, 360 spooled rows | `bench/logs/00_card_inventory-20260918T114838.log` | unknown (pre-dates the probe) | catches | catches | — | not recorded |

**Correction to the brief:** it is not true that any one of the three new tests catches 2043. The monotonic test does not — 2043 is *later* than the last good time. Tightening the year window does not either. **Only the bounded forward jump catches it**, with the temperature tell as an independent second line of defence when the failure is a failed transaction rather than a plausible-looking corrupt register set. That is the argument for shipping all three rather than picking one.

### Evidence from the project's own captures

The 2026-09-10 capture is a wedge caught in the act, and it is the reason to trust the temperature tell:

```
2026-09-10T19:24:12Z Temperature: 17.00
2026-09-10T19:24:12Z PercentExtended: -1
2026-09-10T19:24:12Z 0:0:0
2026-09-10T19:24:12Z 1495506120
2026-09-10T19:24:12Z Status: ACTIVE
2026-09-10T19:24:12Z Set status to: NIGHT
```

- 64 blocks in that capture printed `Temperature: 17.00`. All 64 also printed `0:0:0`, and no `0:0:0` block lacked the 17.00 tell. 64/64, seven episodes, 19:24 → 20:14.
- The garbage clock **changed tracker state**: `Set status to: NIGHT` at 19:24 because the hour read as 0. That is the cost of the current hole, not a hypothetical.
- The recovery path added in `0367716` works. On 2026-09-22 at 08:37 the field unit logged 8 consecutive `I2C bus unhealthy (code …), attempting bus clear #N` / `Bus clear: sda_freed=1 recovered=1` pairs (`src/SolarTracker/SolarTracker.cpp:40-51`), recovered every time, and lost no poll cycles. That capture lives in the home PC's checkout (ssh host `home`), not in this repo — `grep -r 2043` here returns nothing. **What is missing is not recovery. It is detection.**

### Contradiction with the shipped code's own rationale

`src/Clock/Clock.cpp:12-14` states: *"the real failure returns non-zero garbage, never an all-zero time, so the old test never fired."* The captures show both halves of that are wrong. `0:0:0` was printed 64 times on 2026-09-10, and `Power cycling RTC` appears **zero** times in the same file. The real reason `checkForZeroTime()` never fired is structural: `getFullTimeString()` calls `rtc.now()` five times in one printed block (once inside `checkForZeroTime()`, then once each in `getHour()`, `getMinute()`, `getSecond()`, plus `getTimestamp()` on the next line), and each call returns *different* garbage — which is why the same block shows `0:0:0` next to epoch `1495506120` (02:22:00). `wasZeroTime` never latched twice in a row. Requirement 1 exists because of this: one snapshot per decision is both the detection fix and the traffic fix.

Separately, research dimension 2 (claim 16, high confidence, load-bearing) contradicts the standing theory that more radio activity aggravates the wedge: bench run 2026-09-18 did 12,031 RTC samples, 3,588 SD writes and 18 associate/TLS/`WiFi.end()` cycles with zero wedges. So **this PRD must not be sold as reducing the wedge rate.** It reduces the cost of checking, and it stops junk being believed. The fault itself is untouched.

### Reuse rather than rewrite

The detector already exists in this repo, with roughly 12,000 bench samples behind it: `RtcSnap` / `rtcSnapshot()` / `rtcBad()` in `bench/common/bench_common.h:40-74`. Port it into `ClockModule` and add the two temporal tests. Do not write a fresh one.

### Sketch

```cpp
// Clock.h  (additions)
enum ClockFault : uint8_t {
  CLOCK_OK = 0, FAULT_PROBE, FAULT_TEMP, FAULT_YEAR, FAULT_BACKWARD, FAULT_FROZEN, FAULT_FORWARD
};

bool     isHealthy(bool force = false);
uint32_t bestTimestamp();        // validated RTC value, else lastGoodUnix + elapsed
void     noteTimeSet();          // called by setDateTime()/setSimpleTime()
static uint8_t lastFault;

private:
  uint32_t      lastGoodUnix = 0;
  unsigned long lastGoodMs   = 0;
  unsigned long lastCheckMs  = 0;
  bool          lastVerdict  = true;
  uint8_t       failedClears = 0;
  static const unsigned long HEALTH_CHECK_MS = 60000;
  static const uint32_t      FORWARD_SLACK_S = 1800;
```

```cpp
bool ClockModule::isHealthy(bool force)
{
  unsigned long nowMs = millis();
  if (!force && lastCheckMs != 0 && nowMs - lastCheckMs < HEALTH_CHECK_MS)
  {
    return lastVerdict;                       // millis(), never the RTC, drives the gate
  }
  lastCheckMs = nowMs;

  Wire.beginTransmission(0x68);
  lastI2cCode = Wire.endTransmission();
  if (lastI2cCode != 0)            return reject(FAULT_PROBE);

  DateTime now = rtc.now();                   // ONE snapshot, requirement 1
  float t = rtc.getTemperature();
  if (t > 16.999f && t < 17.001f)  return reject(FAULT_TEMP);
  if (now.year() < 2026 || now.year() > 2099) return reject(FAULT_YEAR);

  uint32_t unix = now.unixtime();
  if (lastGoodUnix != 0)
  {
    uint32_t elapsed = (nowMs - lastGoodMs) / 1000;   // unsigned: rollover-safe
    if (unix < lastGoodUnix)                  return reject(FAULT_BACKWARD);
    if (unix == lastGoodUnix && elapsed >= 2) return reject(FAULT_FROZEN);
    if (unix - lastGoodUnix > elapsed + FORWARD_SLACK_S) return reject(FAULT_FORWARD);
  }

  lastGoodUnix = unix; lastGoodMs = nowMs;
  lastFault = CLOCK_OK; lastVerdict = true; failedClears = 0;
  return true;
}

uint32_t ClockModule::bestTimestamp()
{
  if (isHealthy(true)) return rtc.now().unixtime();
  if (lastGoodUnix == 0) return 0;            // nothing trustworthy yet this power session
  return lastGoodUnix + (millis() - lastGoodMs) / 1000;
}
```

**Implementation gotcha that will bite:** `src/SolarTracker/SolarTracker.cpp:43` re-checks health after the clear with `bool back = clockModule.isHealthy();`. Once caching exists that returns the stale `false`, and `recoverCount` stops incrementing forever. It must become `isHealthy(true)`. The 2026-09-22 evidence depends on that counter.

### Choice 1 — how to throttle

| Option | Probes/day (bound) | Notes |
| --- | --- | --- |
| **A. `millis()` cache, 60 s, plus forced checks (recommended)** | 1,440 + 144 forced + 1 per clear ≈ **1,584** | Verdict is never older than 60 s; the row that matters is always freshly checked. Smallest diff. |
| B. Check only inside the 10-min spool | 144 | Cheapest, but a wedge is invisible for up to 10 min and the 10-second serial block keeps printing junk, which is what makes field captures unreadable. |
| C. Check on demand behind a 10 s cache | ~8,640 | Matches the existing 10 s status cadence, 5× the bus traffic of A for no extra decision quality. |

At today's ~1 ms-per-check bound the in-loop check cannot exceed roughly 1,000 evaluations/s, i.e. up to ~86.4 M/day; option A is a reduction of about 5×10⁴. The actual `loop()` rate is unmeasured — requirement for the bench run below.

### Choice 2 — what timestamp a rejected poll spools

| Option | Behaviour | Verdict |
| --- | --- | --- |
| A. Spool the RTC value anyway (today) | 2043 rows land in the bucket and are indistinguishable from truth | The defect |
| B. Skip the row | No junk, but the fault leaves no trace and a data gap looks like a power cut | Rejected |
| **C. Spool `lastGoodUnix + elapsed millis` (recommended)** | Monotone, bounded by RA4M1 crystal drift, and the row still carries temperature, extension and status | Recommended; nothing is written back to the RTC |

### Optional, 15 minutes: read OSF

`rtc.lostPower()` is the DS3231 oscillator-stop flag (status register `0x0F`, bit 7). `bench/README.md` notes the shipped firmware never looks at it outside `setup()`, and that `osf=0` with a bad read means *the chip is fine and the bus failed*. Log it alongside the fault code. Do not give it its own action yet — one behaviour, one PRD.

## Risks

| Risk | Likelihood | Impact | Mitigation |
| --- | --- | --- | --- |
| The DST step from `/time/est` trips the monotonic test. `routes.js:100-105` uses `timeZone: "America/New_York"`, and syncs run hourly (`SYNC_EVERY_HOURS = 1`), so at 02:00 local on **2026-11-01** the RTC is set back 3,600 s | Certain, and dated | Without requirement 8: a bus clear every minute for an hour and every row flagged | Requirement 8 — `noteTimeSet()` clears the reference on every `setDateTime()`/`setSimpleTime()`. Bench-test it by setting the clock backwards by an hour on purpose |
| A bug here stops the tracker tracking and **makes a site visit more likely** — the dominant project risk | Low, but the whole point of the caution | A drive out, and possibly a panel stuck at one angle | Requirement 10 (verdict never gates motion), requirement 7 (forced checks), fault injection on the bench before flashing, and a 12 h soak. Flash from the home PC, which is already attached |
| `busClear()` runs while the motor is energised. `loop()` is re-entered from inside the actuator drive loop, and the clear contains `delay(10) + delay(2) + delay(20)` plus `rtc.begin()` — roughly 35–50 ms of extra motor-on time per clear, 8 clears in one minute on 2026-09-22 | Medium | Panel overshoots its target | Requirement 11 defers the clear until `actuator.status == STOP`; recovery is not urgent at a 60 s cadence. Measure the real `busClear()` duration on the bench |
| Cached verdict hides a wedge that starts just after a check | Medium | Up to 60 s of junk in the 10-second serial block | The telemetry row and the post-clear re-check are always forced; the serial block is diagnostic only |
| Throttling changes the firmware's I²C exposure from ~1 kHz to 1/min, so wedge counts before and after are **not comparable** | High | Confounds the still-open root-cause hunt, and the remaining bench steps (PSU present, then motor moving) lose their baseline | Do the two remaining `bench/README.md` steps **before** this ships, or record the loop rate and probe count in both builds so the rates can be normalised |
| A genuine fault that a clear cannot fix (real OSF, dead coin cell) makes the unit clear the bus forever and spam the log | Low | Log volume; research measured non-poll log lines at 36 B average, so even 1/min is noise, but it buries real signal | Requirement 12 — back off to 10 min after 3 unrecovered clears, one line per clear |
| Flash/RAM regression on a board at 48% flash and 34% global RAM | Very low | None expected: a few hundred bytes of flash, ~16 B of RAM | Requirement 14 — measure with `arduino-cli compile`, do not assume |

## Acceptance criteria

- [ ] A fault-injection bench sketch that forces each of `FAULT_PROBE`, `FAULT_TEMP`, `FAULT_FROZEN`, `FAULT_BACKWARD`, `FAULT_FORWARD` shows each one rejected, with the fault code in the serial line.
- [ ] A synthetic `2043-04-29T18:05:44` reading injected after a good reading is rejected as `FAULT_FORWARD`, and the row spooled for that poll carries `lastGoodUnix + elapsed` instead.
- [ ] The clock set back 3,600 s through `setDateTime()` produces **no** rejection and **no** bus clear.
- [ ] Measured `loop()` iterations/s and I²C transactions/s recorded for the current build and the new build, with the new build's health evaluations at ≤ 1,584/day (1/min plus forced).
- [ ] `busClear()` duration measured in ms on the bench, and no clear observed while `actuator.status != STOP` over a 12 h soak.
- [ ] A 12 h soak on `bench/12_matrix` or `05_combined` shows zero rejections with a healthy RTC — no false unhealthy, no bus clear.
- [ ] `recoverCount` still increments after an induced wedge, proving the forced re-check at `SolarTracker.cpp:43` was not broken by caching.
- [ ] Flash and RAM reported against 127,688 B / 11,212 B; flash under 135,000 B and global RAM under 11,400 B.
- [ ] After one week in the field, no object under `gs://solar-tracker-442903-telemetry/raw/` contains a `time` outside `[boot, now + 30 min]`.
- [ ] The field serial log shows at most one line per bus clear and no repeated clear bursts on a healthy bus.

## Open questions

- Does RTClib really return a `DateTime` built from uninitialised stack when `write_then_read()` fails, and is 17.00 really register `0x11` cast to float? Load-bearing for requirement 4, asserted only by this project's own bench comment (`bench/common/bench_common.h:50`), and the adversarial verification pass did not run. 30 minutes in the RTClib and Adafruit_BusIO sources settles it. The 64/64 field correlation stands either way.
- Where is the 2026-09-22 08:37 capture, and can it be committed? The exact `2043-04-29T18:05:44` value is quoted from that event and is not in this repo (`grep -r 2043` finds nothing). `docs/diagnostics/README.md` exists precisely because Cloud Logging retains 30 days — this one should be committed before 2026-10-22.
- What is the actual `loop()` iteration rate? Everything about the traffic saving is currently a bound, not a measurement.
- Does the coin cell still hold? If OSF is genuinely setting, the response is a battery, not software, and the bus clear is the wrong reflex.
- Should the fault code become visible in the cloud? It cannot without a ninth column, and the server drops any row that is not exactly 8 columns. That is a separate PRD; until then the fault code is serial-only and invisible from the field.
- Should the two remaining bench steps (12 V PSU present, then actuator moving) run before this lands, to keep the root-cause baseline intact?
