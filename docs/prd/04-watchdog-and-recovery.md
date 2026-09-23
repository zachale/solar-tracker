# 04 — Watchdog and unattended recovery

**Status:** Proposed
**Owner:** unassigned
**Depends on:** nothing. Buildable and bench-testable today: no bridge firmware version, no broker, no MQTT decision. The one server change is a new route that does not touch `/data`.
**Blocks:** 05 and 06 — both add network activity, therefore hang surface, and neither should go to the field unattended before this lands. 02 is not blocked, but a bad OTA image is only survivable if this exists first.
**Estimate:** 16–20 h of build (2–2.5 working days), then a 72 h unattended bench soak before the field flash. Stage 2 (letting the ladder actually reboot) is +1 h, two weeks later.
**Cost:** none. One extra `POST /diag` per sync = 24/day = 720/month, against Cloud Run's 2,000,000 free requests/month. Storage: 720 records × ~200 B = 144 KB/month = 0.000144 GB × $0.020/GB-month = **$0.000003/month**. GCS Class A writes go from ~720 to ~1,440 per month, against a 5,000/month free tier ([GCS pricing](https://cloud.google.com/storage/pricing)).

**Do first, in this order:**

1. `grep -rin "watchdog\|WDT\|NVIC_SystemReset" src/ solar-tracker.ino` → confirm it still returns nothing.
2. Flash a 10-line sketch that calls `WDT.begin(5000)` and prints `WDT.getTimeout()`. If it does not print `5592`, every number in this document needs redoing.
3. Force a watchdog reset ten times on the bench and confirm the board comes back into the sketch every time, not into the bootloader. If it ever lands in the bootloader, stop — this feature would cause the site visit it is meant to prevent.

## Problem

There is no watchdog in this firmware: `grep -rin "watchdog\|WDT\|NVIC_SystemReset"` across the repo returns nothing, so every hang is permanent until a human power-cycles the board. The blocking is real and measured, not theoretical — on 2026-09-09 a single `GET /time/est` held the loop for **125 seconds** (`docs/diagnostics/serial-2026-09-09.log:74-76`, `14:00:07Z` to `14:02:12Z`) and then printed `Request Failed: 200`, because `WifiClient::get()` retries ten times whenever the body is empty even though the status was 200 every time (`src/WifiClient/WifiClient.cpp:69-85`, with `http.setTimeout(15000)` at line 59: a 150-second ceiling). Underneath that, any single AT command can stall the RA4M1 for 10 s (`MODEM_TIMEOUT` at `renesas_uno/1.6.0/libraries/WiFiS3/src/Modem.h:12`), a failed `Wire.endTransmission()` costs roughly 1 s ([core issue #492](https://github.com/arduino/ArduinoCore-renesas/issues/492)), and a hard fault hangs forever in `Fault_Loop: BL Fault_Loop` (`cores/arduino/cm_backtrace/fault_handler/gcc/cmb_fault.S:44-45`, active because `boards.txt:122` builds this board with `-DBACKTRACE_SUPPORT`).

What that costs today: the only thing standing between a wedged tracker and a drive to site is the USB cable to Zach's home PC. Every feature in 02, 05 and 06 adds network calls and therefore adds ways to stop coming back.

## Goal

Any state this firmware can get stuck in returns to a running sketch within two minutes without a human, and the next cloud record says why it rebooted.

## Non-goals

- Does not find the DS3231 wedge root cause. That is the bench ladder (PSU connected, then motor moving) in `bench/README.md` → "Next experiment".
- Does not change `ClockModule::isHealthy()`, its every-loop cadence (`solar-tracker.ino:94` → `SolarTracker.cpp:37`) or its year-2043 hole. Separate PRD.
- Does not widen the 8-column telemetry row, and does not touch `parseRow()`.
- Does not add OTA (02), MQTT (05) or log shipping (06). It makes them safe to deploy; it does not build them.
- Does not add external watchdog hardware, and does not modify the ESP32 bridge firmware.
- Does not wipe the SD card, reformat it, reset the RTC, or delete any file it did not create.

## Requirements

1. Enable the RA4M1 hardware watchdog through the core's `WDT` library at the longest period the part grants, check the return value of `WDT.begin()`, and print the granted period on the first line of every boot.
2. Refresh the hardware watchdog **only** from a periodic timer ISR, and only after that ISR has confirmed `loop()` reported progress inside the current deadline.
3. Claim the supervisor timer with `FspTimer::get_available_timer(type)` with `force = false`, refuse AGT channel 0, and fail loudly rather than take a channel the actuator's D5/D6 PWM needs.
4. Latch the reset cause from `RSTSR0/1/2` before anything else in `setup()`, print it, then clear the flags so the next boot reports its own cause.
5. Set the progress deadline to 240 s during `setup()`, 90 s while the settings switch is on, and 30 s in normal field operation.
6. Bound `WifiClient::get()` to 3 attempts at an 8 s per-request timeout, and give `dailySync()` a 120 s wall-clock budget after which remaining chunks stay on the card.
7. Report progress at every proven forward step of a sync, so a stalled single request trips the deadline while a slow-but-advancing sync does not.
8. Reboot via `NVIC_SystemReset()` when `WiFi.status() == WL_NO_MODULE`, or when `WiFi.firmwareVersion()` returns `99.99.99` (unverified: one forum report, and the adversarial verification pass on the research never completed).
9. Reboot after 3 consecutive failed syncs (unverified: the socket/heap-exhaustion mechanism behind the field reports is inferred from Espressif's ESP-TLS documentation, not observed on this unit).
10. Refuse every deliberate reboot while `actuator.status != STOP`, while status is `SAFE`, while the settings switch is on, and within 30 minutes of boot.
11. Close every open `File` before a deliberate reboot, and never issue one from inside an upload.
12. Post one diagnostics record per sync to a new `POST /diag` route carrying free RAM, uptime, boot count, reset cause, granted WDT period, late-tick count, the association/socket/failure ledger, `WiFi.status()`, RSSI, the bridge firmware string, and the wedge/recover counts.
13. Leave the 8-column `/data` row and `parseRow()` byte-identical, so no telemetry row can be lost to this work.
14. Ship in two flashes: stage 1 is hardware watchdog + bounds + reporting; stage 2, two weeks later, enables the deliberate-reboot ladder.
15. Keep the build within 2 KB of flash and 200 B of globals over the 127,688 / 11,212 byte baseline.
16. Prove each recovery path on the bench with an injected fault — 45 s stall, hard fault, SDA held low, black-hole API address — before the field flash.

## Design

### The obvious design does not work, and here is the arithmetic

The core ships a watchdog library (`renesas_uno/1.6.0/libraries/WDT/`, global `WDT` object at `WDT.h:114`). Its maximum period is fixed by the part:

```
getMaxTimeout() = (RL_16384 × PR_8192) / (PCLKB_Hz / 1000)      WDT.cpp:110-113
PCLKB           = HOCO 48 MHz / 2 = 24 MHz                      bsp_clock_cfg.h:8,14
                = (16384 × 8192) / 24000
                = 134,217,728 / 24,000
                = 5,592 ms
```

**5,592 ms is the ceiling, and `MODEM_TIMEOUT` is 10,000 ms.** A single legal AT command — one that is slow, not broken — is nearly twice the longest watchdog period this chip can be given. `WDT.begin()` silently returns `0` for anything longer (`WDT.cpp:54-57`), which means a naive `WDT.begin(30000)` leaves the board with no watchdog at all and no error. There is no independent watchdog to fall back to: the variant ships no IWDT driver at all (`find variants/UNOWIFIR4 -iname '*iwdt*'` → nothing), so its longer ~35 s period is not reachable from this toolchain.

Two useful facts follow from the same files. `OFS0` has bit 17 (`WDTSTRT`) set (`bsp_mcu_family_cfg.h:33`), which selects register-start mode: the watchdog is **off after every reset** until the first refresh. So a board sitting in the bootloader, or one with a failed flash, is never watchdog-reset — a failed upload leaves an unprotected board, not a reset loop. And `Serial` on this board is `_UART1_`, not USB CDC (`boards.txt:122` defines `NO_USB`, `Arduino.h:113`), with `UART::operator bool()` returning `true` unconditionally (`Serial.cpp:141-144`), so the `while (!Serial);` at `solar-tracker.ino:57` cannot stall a boot with no host attached.

### Options

| Option | Detection window | Cost | Verdict |
| --- | --- | --- | --- |
| **A.** `WDT.refresh()` at the top of `loop()` | 5,592 ms, hard ceiling | trivial | **Rejected.** One legal 10 s AT command resets the board mid-POST. 02's OTA download raises the modem timeout to 60 s (`Modem.h:13`), which would reset it mid-flash-download. |
| **B.** Hardware WDT refreshed from a 1 Hz ISR that is gated on main-loop progress | any deadline you choose; hardware backstop fires ≤5.6 s after the ISR stops kicking | ~1 KB flash, one GPT channel, one ISR | **Recommended.** |
| **C.** Software only: measure elapsed time in `loop()`, call `NVIC_SystemReset()` | never fires, because the hang is inside a core call that `loop()` cannot return from | trivial | **Rejected as the mechanism.** Kept as the ladder for "running but useless" (below), where the loop is healthy and the radio is not. |

An ISR-kicked watchdog is normally an anti-pattern because it hides hangs. It does not here, because the kick is conditional on the main loop having advanced, and because the ISR itself is the second thing being tested: a hard fault runs at priority −1 and blocks the priority-12 supervisor ISR, so `Fault_Loop` stops the kicks and the board resets in ≤5.6 s instead of hanging forever. The same applies if interrupts are lost altogether — `millis()` is driven by the AGT0 underflow ISR (`cores/arduino/time.cpp:22-46`), so a dead NVIC freezes `millis()` *and* the refresh path, and the hardware timer wins.

### The supervisor

```cpp
// src/Watchdog/Watchdog.h
//
// Two watchdogs in one. The RA4M1's hardware WDT can only be set to 5,592 ms on
// this part, which is shorter than a single legal AT command, so the hardware
// timer is refreshed from a 1 Hz interrupt and the number we actually care about
// lives in software: the interrupt refuses to refresh once the main loop has been
// silent for longer than the deadline. A ten second AT command therefore cannot
// reset the board, and a loop that never comes back cannot avoid it.
class Watchdog
{
public:
  static bool begin();                        // first call in setup(); false = nothing armed
  static void progress();                     // "the main loop advanced"
  static void setDeadline(unsigned long ms);  // 240 s boot, 90 s app mode, 30 s field
  static void allow(unsigned long ms);        // one known-slow call (OTA download)

  static uint32_t grantedPeriodMs();
  static uint16_t resetCause();               // bitfield latched at boot
  static uint16_t lateTicks();                // refreshes the ISR withheld

  static bool rebootAllowed();
  static void rebootNow(const char *reason);  // does not return
};
```

```cpp
// src/Watchdog/Watchdog.cpp  (sketch)
#include <WDT.h>
#include "FspTimer.h"

static const uint32_t WDT_REQUEST_MS   = 5000;    // granted as 5,592
static const unsigned long BOOT_DEADLINE_MS = 240000;

static FspTimer supervisor;
static volatile unsigned long lastProgress = 0;
static volatile unsigned long deadline     = BOOT_DEADLINE_MS;
static volatile uint16_t      lateTickCount = 0;

static void supervisorTick(timer_callback_args_t *)
{
  if (millis() - lastProgress <= deadline) {
    WDT.refresh();
  } else {
    lateTickCount++;      // stop kicking: the hardware resets us ≤5,592 ms from here
  }
}

bool Watchdog::begin()
{
  latchResetCause();
  lastProgress = millis();

  if (WDT.begin(WDT_REQUEST_MS) == 0) {
    Serial.println("WATCHDOG NOT ARMED: WDT.begin() refused the period");
    return false;
  }

  // force = false on purpose. variant.cpp:126-131 pre-marks the GPT channels
  // behind D3/D5/D6/D9/D10/D11 as TIMER_PWM, and get_available_timer() only
  // hands out TIMER_FREE channels (FspTimer.cpp:133-145), so the actuator keeps
  // the two channels it drives. force = true would take one of them and the
  // panel would stop moving.
  uint8_t type = GPT_TIMER;
  int8_t  ch   = FspTimer::get_available_timer(type);
  if (ch < 0 || (type == AGT_TIMER && ch == 0)) {
    // AGT0 is millis(). FspTimer.cpp:15-16 has the line that would have marked it
    // used commented out, so it is advertised as free. Refuse it.
    Serial.println("WATCHDOG NOT ARMED: no safe timer channel");
    return false;
  }

  Serial.println("Watchdog armed: period=" + String(WDT.getTimeout()) +
                 "ms timer=" + String(type == GPT_TIMER ? "GPT" : "AGT") +
                 String(ch) + " cause=0x" + String(resetCause(), HEX));

  return supervisor.begin(TIMER_MODE_PERIODIC, type, (uint8_t)ch, 1.0f, 0.0f, supervisorTick)
      && supervisor.setup_overflow_irq()
      && supervisor.open()
      && supervisor.start();
}
```

### Where `progress()` goes

| Call site | Why there |
| --- | --- |
| `loop()`, first line (`solar-tracker.ino:94`) | the normal heartbeat; also covers `setup()`, because `home()`, `max()` and `moveTo()` call back into `loop()` on every iteration of their drive loops (`LinearActuator.cpp:57`, `:80`, `:214`) |
| `WifiClient::ensureWifiConnection()`, inside the `delay(1000)` loop | association legitimately takes ~6 s per SSID and burns a full timeout on `Rock Lodge` every sync (`bench/README.md`, incidental finding 1) |
| `WifiClient::get()` / `post()`, after each attempt returns | a *stalled* request trips the deadline; a slow one that keeps returning does not |
| `SolarTracker::uploadLogs()`, after each confirmed chunk | a ten-chunk drain is forward progress, not a hang |

Nothing pumps `progress()` inside a retry loop that has no exit — every pump point sits after a bounded call has returned.

### Reset cause

The RA4M1 latches why it last reset, nothing in the core reads or clears it (`grep RSTSR cores/` finds only the device header), so the flags are still standing when the sketch starts:

```cpp
// R7FA4M1AB.h:9684-9712 (RSTSR1: IWDTRF/WDTRF/SWRF), :10202-10231 (RSTSR0 PORF, RSTSR2 CWSF)
enum { RESET_POWER_ON = 1, RESET_WATCHDOG = 2, RESET_SOFTWARE = 4, RESET_COLD = 8 };

static void latchResetCause()
{
  if (R_SYSTEM->RSTSR0_b.PORF)      causeBits |= RESET_POWER_ON;
  if (R_SYSTEM->RSTSR1_b.WDTRF)     causeBits |= RESET_WATCHDOG;   // the watchdog fired
  if (R_SYSTEM->RSTSR1_b.SWRF)      causeBits |= RESET_SOFTWARE;   // NVIC_SystemReset()
  if (R_SYSTEM->RSTSR2_b.CWSF == 0) causeBits |= RESET_COLD;

  rawRstsr[0] = R_SYSTEM->RSTSR0;   // report the raw words too, so a clear that
  rawRstsr[1] = R_SYSTEM->RSTSR1;   // does not stick is visible rather than silent
  rawRstsr[2] = R_SYSTEM->RSTSR2;

  R_SYSTEM->PRCR = (uint16_t)BSP_PRV_PRCR_PRC1_UNLOCK;  // same unlock the variant
  R_SYSTEM->RSTSR0 = 0;                                 // uses at variant.cpp:95-98
  R_SYSTEM->RSTSR1 = 0;
  R_SYSTEM->RSTSR2_b.CWSF = 1;
  R_SYSTEM->PRCR = (uint16_t)BSP_PRV_PRCR_LOCK;
}
```

Which PRCR group protects `RSTSR` is unverified — read the words back after the write and report both. If the clear does not stick, the cause is still reported correctly on the first boot after the event, and only the stickiness is lost.

### Bounding the network

| Change | File | From | To |
| --- | --- | --- | --- |
| GET attempts | `WifiClient.cpp:69` | 10 | 3 |
| Per-request timeout | `WifiClient.cpp:59,118` | 15,000 ms | 8,000 ms |
| `delay(100)` between attempts | `WifiClient.cpp:72` | blind sleep | `Watchdog::progress()` |
| Sync wall-clock budget | `SolarTracker::dailySync()` | none | 120,000 ms, checked before each chunk |
| Credential order | `wifi.jsn` on the card | `Rock Lodge` first, never associates | working SSID first |

Worst legitimate sync after this: ~6 s association + 3 × 8 s GET + N × 8 s POST, capped at 120 s. That is what the 30 s deadline plus per-step `progress()` is sized against — and it replaces a path that was measured at 125 s for one GET.

Leaving chunks on the card when the budget expires is safe and already the design: `uploadLogs()` only clears a chunk on a confirmed 200 (`SolarTracker.cpp:286-303`), and `Telemetry.h:13-17` explains why whole-file deletion was chosen over rewriting.

### The escalation ladder

| Symptom | Detected by | Action | Why it stops there |
| --- | --- | --- | --- |
| `loop()` silent past the deadline | supervisor ISR | stop kicking → hardware reset ≤5,592 ms later | the only way out of a blocked core call |
| Hard fault | priority −1 handler starves the priority-12 ISR | same | `cmb_fault.S:44-45` otherwise loops forever |
| `WL_NO_MODULE` | `WiFi.status()` before a sync | log, then reboot if the fuse allows | the modem link is gone; nothing in software reaches past it |
| `firmwareVersion() == "99.99.99"` | string compare before a sync | reboot if the fuse allows | the [only reported workaround](https://forum.arduino.cc/t/arduino-uno-r4-wifi-connection-losses-wifi-fw-version-is-reported-as-99-99-99-instead-of-0-4-1-after-reconnection/1276079) is exactly this, sometimes twice — **unverified on this unit** |
| 3 consecutive failed syncs | counter in `SolarTracker` | `WiFi.end()` on the next attempt (already the behaviour at `WifiClient.cpp:26`), then reboot | matches the [socket-leak report](https://forum.arduino.cc/t/wifi-stops-connecting-after-a-while/1204442): thousands of good SSL connections, then nothing until a power cycle |
| Still failing after a reboot | boot count and failure count in the diag record | **do nothing more.** Keep tracking, keep spooling | the RA4M1 cannot power-cycle the ESP32-S3. This is the case that is a site visit, and the diag record is how it gets recognised early |

The fuse on every deliberate reboot: not moving, not `SAFE`, switch off, and uptime ≥ 30 min. The 30 minutes matters more than it looks — `maxPos` starts at 0, so the first `extendToPercent()` after any boot runs `recalibrate()`, which is `delay(5000)` plus a full home-and-max sweep (`LinearActuator.cpp:160-163`, `:39-45`). A reboot is not free; it costs a motor sweep, and a sweep in high wind is precisely what the comment at `SolarTracker.cpp:122-133` was written to prevent.

### What cannot be measured, and one correction

"Instrument ESP32 socket state" is not available. `WiFiCommands.h` has no command for bridge heap or for a socket-slot census, and the bridge is built with `PSRAM=disabled` on an ESP32-S3-MINI-1-N8 with no way to report free heap. What is observable is our own ledger plus three cheap probes:

- ours: SSL opens, SSL closes, connect failures, `WiFi.begin()` attempts, `WiFi.end()` calls, consecutive sync failures, wedge and recover counts (already counted at `Clock.h:34-36`)
- the bridge's: `WiFi.status()`, `WiFi.RSSI()`, `WiFi.firmwareVersion()`
- ours again: free RAM via the `sbrk` helper the bench harness already uses (`bench/common/bench_common.h:23-28`), uptime, boot count, granted WDT period, late ticks

An opens-minus-closes ledger that drifts is the closest thing to a socket-leak alarm this board can have, and it costs nothing.

### Getting it to the cloud without breaking telemetry

The deployed server drops any row that is not exactly 8 columns and answers 200 anyway (`server/routes.js:45-47` on `feature/telemetry-api`, which is what is running). So diagnostics cannot ride in the telemetry row, and they must not be smuggled into an existing field — that is how wedge counts ended up inside `wind_peak`, selected by `digitalRead(D4)`, where a floating pin silently reverts the field to wind data (`SolarTracker.cpp:154-158`).

One new route, `/data` untouched:

```js
// server/routes.js — feature/telemetry-api
app.use(express.text({ type: "text/plain", limit: "64kb" }));

app.post("/diag", async (req, res) => {
  const lines = String(req.body || "").split("\n").filter((l) => l.trim() !== "");
  if (lines.length === 0) return res.status(200).send({ saved: 0 });
  const now = new Date();
  const object = `diag/${now.toISOString().slice(0, 10)}/${now.toISOString().replace(/[:.]/g, "-")}.ndjson`;
  const ndjson = lines.map((l) => JSON.stringify({ received: now.toISOString(), line: l })).join("\n") + "\n";
  await storage.bucket(TELEMETRY_BUCKET).file(object).save(ndjson, { contentType: "application/x-ndjson" });
  res.status(200).send({ saved: lines.length, object });
});
```

Device side: one `key=value` line per sync, in the shape the bench harness already uses, appended to a new `diag.csv` on the card, POSTed **after** the telemetry chunks so telemetry keeps priority, deleted on a confirmed 200, and discarded if it grows past 8,192 bytes unsent. Diagnostics are the only data in this system that may be dropped to protect something else.

```
boot=3 up=41231 cause=0x02 rstsr=00,0002,0001 wdt=5592 late=1 ram=19864 \
sync=17 ok=15 fail=2 consec=0 assoc=19 open=34 close=34 cfail=1 \
wifi=3 rssi=-71 fw=0.4.1 wedge=8 recov=8
```

~200 bytes, 24/day, 4.8 KB/day. Nothing about the existing telemetry path changes.

### Why this lands before 05 and 06

05 keeps a TLS socket open permanently and polls it with an AT round-trip; 06 multiplies the number of requests per day. Both remove slack from a system that has never had a recovery path at all. Three specifics:

- The closest published analogue to 05 is Arduino IoT Cloud on this exact board — a persistent MQTT-over-TLS session through this same bridge — and it is [widely reported](https://forum.arduino.cc/t/iot-cloud-disconnect-after-a-few-hours-uno-r4-wifi/1280222) to drop after 3–12 hours with **no automatic recovery, requiring a manual reset**. Shipping that to a remote site without a watchdog is shipping a scheduled site visit.
- Today's reliability comes from closing the socket on every request and soft-resetting the radio on every reconnect. A persistent session deliberately removes both, so the failure mode moves from "this sync failed, retry in an hour" to "nothing works until someone drives out".
- 06's whole value is telling you *why* the tracker is unwell. A reset-cause byte and a free-RAM figure are the two most diagnostic numbers the device has, and this PRD produces them. 06 can then carry them with everything else.

There is one pairing worth noting: 02's boot-time OTA check plus this PRD's reboot is the closest thing to remote repair this hardware can have. A hung unit reboots itself, and the boot check then looks for a new image. Neither half works alone — the ESP32 never retries on its own, and re-entering the bootloader needs the 1200-bps USB touch only an attached laptop can send.

### Where this contradicts the project's own docs

`docs/ota-esp32-bridge.md:5-9` frames OTA as the way out: "every firmware change so far has needed a laptop plugged into the board. An update the tracker can fetch for itself would remove that." `SPEC.md` says the same of the server. That is true for *planned* changes and false for *failures*, and the difference is the whole reason this PRD exists. The bridge verifies the image's magic and CRC32, never that the new sketch runs; it is a passive AT slave that only acts when the RA4M1 asks; and `BossaUnoR4WiFi::program` discards `flash()`'s return value and returns 1 regardless ([source](https://github.com/arduino/uno-r4-wifi-usb-bridge/blob/main/UNOR4USBBridge/BossaUnoR4WiFI.cpp)). A sketch that flashes cleanly and then hangs is, today, a drive to site. OTA without a watchdog raises the stakes of every update; the watchdog is what makes OTA worth attempting.

Smaller correction from the same doc: line 12 states "The current firmware is 126,944 bytes". It is 127,688 bytes now (48% of 262,144, verified with `arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi`). The conclusion is unchanged — still over the 122,880-byte `InternalStorage` ceiling — but the number has drifted 744 bytes in nine days, which is why requirement 15 puts a 2 KB budget on this work.

## Risks

| Risk | Likelihood | Impact | Mitigation |
| --- | --- | --- | --- |
| Watchdog fires during a legitimately slow operation, and keeps doing it → reset loop → **site visit more likely, not less** | Medium before soak, low after | High | Deadline sized off measured data, not this document's guess; `allow()` for known-slow calls; 72 h soak must show `late=0`; stage 1 ships with reporting only, so a bad deadline shows up as `late` counts in the cloud before anything reboots |
| Two rapid resets drop the board into bootloader mode → dead until USB → **site visit** | Low | Very high | The "do first" bench test: ten forced watchdog resets, sketch must come back every time. 30-minute floor between deliberate reboots. WDT is off after reset (`OFS0` bit 17), so a board in the bootloader is never re-reset |
| Every reboot costs a full home-and-max sweep (`maxPos == 0` → `recalibrate()`), and a sweep in wind is the thing `serviceSync()` guards against | Medium | Medium | Never reboot deliberately while `SAFE`, while moving, or within 30 min of boot. Hardware resets are not fusable, which is another reason the deadline must be generous |
| Watchdog reset lands mid-SD-write → truncated final row, or rarely FAT damage that also costs `wifi.jsn` | Low | High (the card holds the WiFi credentials) | Writes are one `open`/`print`/`close` per row (`Telemetry.cpp:111-121`); no file is held open across a network call; deliberate reboots only from the top of `loop()` with everything closed. A truncated row is already dropped cleanly by `parseRow()` |
| Supervisor steals a timer channel the actuator's PWM needs → panel never moves | Low | Very high | `get_available_timer(force=false)` only takes `TIMER_FREE`, and `variant.cpp:126-131` pre-marks D3/D5/D6/D9/D10/D11; refuse AGT0; log the channel; bench-verify motion at full and reduced speed before flashing |
| The `99.99.99` trigger is one forum report — it may never fire, or the real symptom may differ | Medium | Low | Stage 1 counts it without acting. Enable the reboot in stage 2 only if the count is plausible. Either way it is a `firmwareVersion()` string compare, not a behavioural guess |
| Settings switch on: a slow phone request trips a 30 s deadline and drops the AP mid-configuration, in front of whoever is standing there | Medium | Low | Deadline goes to 90 s while the switch is on, and the deliberate-reboot ladder is disabled in app mode |
| Watchdog masks the DS3231 bug by rebooting through it, confounding the open root-cause hunt | Medium | Medium | Reset cause and wedge counts are in every diag record, so a reboot is never invisible. The two remaining bench steps (PSU present, motor moving) stay ahead of any field change |

## Acceptance criteria

- [ ] Boot line reads `Watchdog armed: period=5592ms timer=GPT<n> cause=0x<..>`, and `period` is not 0.
- [ ] A bench build with `BENCH_HANG_MS=45000` busy-waiting inside `loop()` resets the board within deadline + 6 s, and the next boot reports `cause=0x02` (`WDTRF`). Ten out of ten, with the sketch running afterwards every time — never the bootloader.
- [ ] A deliberate hard fault behind a bench flag resets within 6 s instead of sitting in `Fault_Loop` forever.
- [ ] With SDA held low on the `13_busclear` rig, the board either recovers or resets; the serial log never simply stops.
- [ ] With `API` pointed at a black-hole address, one sync completes and returns within 120 s, spool intact, no reboot; `Request Failed` appears once, not ten times.
- [ ] 72 h unattended bench soak: `late=0`, no unexpected resets, free RAM flat against the ~19.9–21.7 KB the bench runs already show.
- [ ] Actuator drives to 0%, 50% and 100% after the supervisor timer is armed, at both full and reduced speed.
- [ ] `arduino-cli compile` reports ≤ 129,688 bytes flash and ≤ 11,412 bytes globals.
- [ ] `gs://solar-tracker-442903-telemetry/diag/<day>/` holds one record per sync for 24 h, and `/data` objects for the same window still show `saved == rows, skipped == 0`.
- [ ] One cloud diag record, picked at random, correctly reports a reset that was caused by hand at a known time.

## Open questions

- Does a watchdog reset always re-enter the sketch, or can two resets inside the bootloader's double-tap window land it in bootloader mode? This is the one question that could turn this feature into the site visit it is meant to prevent. Answer it on the bench before writing anything else.
- Is `99.99.99` real on this unit? One forum report, `loadBearing: false` in the research, and the adversarial verification pass never completed. Stage 1 counts it; stage 2 decides.
- Which PRCR group protects `RSTSR0/1/2`, and does the clear stick? Read the words back and report them.
- What does `AT+RESET` do on the bridge? It is defined at `WiFiCommands.h:24` and used nowhere in `WiFiS3`, so it is reachable only through `modem.write()`. If it resets the ESP32 rather than the RA4M1, the ladder gains a rung and the last site-visit case may disappear.
- What is the longest legitimate sync once the bounds land? Measure over 48 h and set the deadline from that number rather than from the 30 s in this document.
- Should stage 2 ship at all, or is a hardware watchdog plus reporting enough? A watchdog cannot fix a wedged bridge; the deliberate ladder is a bet that a fresh RA4M1 re-`begin()`s the modem out of it, and that bet is based on one forum thread.
- Does the `isHealthy()` cadence fix land first? While the bus is wedged, that call burns ~1 s per loop iteration *inside the motor-on callback path*. The supervisor sees healthy progress the whole time. The watchdog does not protect position control; only that fix does.
- Does the `sbrk`-based free-RAM helper report the same figures in the shipping build as on the bench, where the heap is smaller? Confirm before trusting the number as a leak alarm.
