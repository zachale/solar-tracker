# Solar Tracker — Specification Sheet

Reverse-engineered from the code at `main` (2c8ac78). This documents what the system
*currently does*, the requirements that behaviour implies, and the gaps between the two.

---

## 1. What this is

A single-axis, **time-based** solar tracker for an off-grid installation, plus a small
cloud service that supports it.

The panel array is tilted by a 12V linear actuator. The firmware does not look at the sun
— it reads a battery-backed real-time clock and moves the actuator to a scheduled
extension percentage once per hour. A wind sensor overrides the schedule and parks the
array when it gets gusty. A physical switch panel lets a technician on site put the unit
into a local configuration mode that serves a mobile-friendly web dashboard over its own
Wi-Fi access point.

The Node service exists because the hardware is remote and hard to reach: it hands out
the correct time (the RTC drifts and loses power), serves over-the-air firmware images so
the unit can be updated without a site visit, and accepts telemetry.

Stated goal (README): **+25% solar output** versus a fixed array, in harsh off-grid
conditions.

### Components

| Component | Language / runtime | Role |
| --- | --- | --- |
| `solar-tracker.ino` + `src/` | C++ (Arduino) | Device firmware, runs on an UNO R4 WiFi |
| `server/` | Node 20 / Express / Mongoose | Time source, OTA update host, telemetry sink |
| `server/Dockerfile` | Docker (node:alpine3.19) | Deployment unit for the service |

---

## 2. Hardware inventory (inferred from pin usage)

| Pin | Direction | Device |
| --- | --- | --- |
| D2 | Input, interrupt RISING | Linear actuator hall-effect pulse (step counting) |
| D3 | `INPUT_PULLUP`, interrupt FALLING | DS3231 `INT/SQW` alarm line |
| D4 | `INPUT_PULLUP` | Wind-sensor enable switch |
| D5 | PWM out | Motor driver — retract / backward |
| D6 | PWM out | Motor driver — extend / forward |
| D7 | `INPUT_PULLUP` | Settings-server enable switch |
| D8 | Output | DS3231 power rail (software power-cycled at boot) |
| A3 | Analog in | Anemometer, 0.4–2.0 V analog output |
| SDA/SCL | I²C | DS3231 RTC (temperature sensor read from the same chip) |

- **MCU:** Arduino UNO R4 WiFi. Required — the code depends on `WiFiS3`, `R4HttpClient`,
  and `InternalStorage` self-flashing, none of which exist on other boards.
- **Actuator feedback:** pulse counting only. No absolute encoder, no limit switches.
  End stops are detected by *stall* (pulses stop arriving for >1 s).

### Firmware dependencies

`RTClib`, `ArduinoJson`, `WiFiS3`, `R4HttpClient`, `ArduinoHttpClient`, `ArduinoOTA`
(for `InternalStorage` only — `NO_OTA_NETWORK` is defined).

A `secrets.h` at the repo root is required and git-ignored. It must define:

- `wifiCredentials` — an iterable of `std::pair<const char*, const char*>` (SSID, password)
- `API` — the service hostname, used as a bare host (no scheme; `WifiClient` prepends
  `https://` and `OTAUpdater` passes it straight to `HttpClient` with port 443)

---

## 3. Functional requirements

### 3.1 Tracking

- **FR-1** The array position shall be derived from wall-clock time, not from a light
  sensor.
- **FR-2** The schedule shall be a three-point curve: a start hour, a middle hour, and an
  end hour, each with a target extension percentage. Positions between points are linearly
  interpolated.
  - Defaults: 08:00 → 20%, 12:00 → 66%, 20:00 → 80%.
- **FR-3** The tracker shall re-evaluate and move the actuator once per hour, driven by
  the RTC alarm interrupt (DS3231 Alarm 1, minute-match on minute 0).
- **FR-4** Movement shall only occur while status is `ACTIVE`.

### 3.2 Actuator control

- **FR-5** The actuator shall support absolute positioning by percentage, where 0% is the
  fully retracted (home) stop and 100% is the fully extended stop.
- **FR-6** Position shall be tracked by counting hall-effect pulses, debounced to a 500 µs
  minimum interval.
- **FR-7** The travel range shall be learned by a calibration routine that drives to both
  stops and records the pulse count between them.
- **FR-8** Calibration shall run automatically when a move is requested and no range is
  known, and shall be manually triggerable from the dashboard.
- **FR-9** A move shall slow down as it approaches target (PWM duty tapers once within 255
  pulses).
- **FR-10** A move in progress shall be interruptible: a new target set mid-travel
  redirects the current move rather than queueing behind it. This is done by calling the
  main loop as a callback from inside the movement loop.
- **FR-11** Hitting an unexpected stop mid-move shall abort, recalibrate, and retry once.

### 3.3 Wind safety

- **FR-12** Wind speed shall be read from an analog anemometer and linearly mapped:
  0.4 V → 0 m/s, 2.0 V → 32 m/s. Below 0.4 V reads as 0.
- **FR-13** When wind stays above the upper threshold for the upper dwell time, the tracker
  shall enter `SAFE` and park the array.
  - Defaults: 24 m/s (86.4 km/h) sustained for 6 s.
- **FR-14** When wind stays below the lower threshold for the lower dwell time, the tracker
  shall return to `ACTIVE`.
  - Defaults: 16 m/s (57.6 km/h) sustained for 90 s.
- **FR-15** The wind sensor shall be defeatable by a physical switch. Disabled, it reports
  0 m/s and never triggers `SAFE`.

### 3.4 State machine

| Status | Entered when | Actuator action |
| --- | --- | --- |
| `ACTIVE` | Inside active hours, wind below threshold | Track the hourly schedule |
| `NIGHT` | Outside active hours (`hour < start` or `hour > end`) | Hold position |
| `SAFE` | Sustained high wind | Move to the "half day" park position |
| `AWAY` | Set manually from the dashboard | Move to the "half day" park position |

- **FR-16** Status shall be re-evaluated on the 10-second sensor poll.
- **FR-17** Initial status at boot shall be `ACTIVE` or `NIGHT` based on the clock.

### 3.5 Time and connectivity

- **FR-18** The device shall sync its RTC from the service once per day, at the 08:00 alarm.
- **FR-19** Time shall be requested from `GET /time/est` and applied as an ISO datetime
  string.
- **FR-20** The Wi-Fi client shall try each configured credential pair in turn, remember the
  one that worked, and short-circuit if already connected.
- **FR-21** HTTP GETs shall be retried up to 10 times before failing; a total failure
  disconnects Wi-Fi so the next attempt starts clean.
- **FR-22** The daily sync shall be skipped while the settings server is enabled — the radio
  cannot be an access point and a station at the same time.

### 3.6 Over-the-air update

- **FR-23** On each daily sync the device shall ask the service whether the *next* version
  number exists (`GET /update/check/{VERSION+1}`).
- **FR-24** If it does, the device shall download `update-v{VERSION+1}.bin` over HTTPS,
  write it to internal storage, and self-flash.
- **FR-25** The version is a compile-time constant (`OTAUpdater::VERSION`). Each published
  image must bump it, or the device will re-download the same update forever.
- **FR-26** Updates shall abort safely on a non-200 response, a missing `Content-Length`,
  insufficient flash, or a mid-download timeout.

### 3.7 On-site settings server

- **FR-27** With the settings switch on, the device shall create an **open** Wi-Fi access
  point named `Solar-Tracker` and serve an HTTP dashboard on port 80.
- **FR-28** The dashboard shall display live wind speed, extension percentage, time, and
  status.
- **FR-29** The dashboard shall allow, via HTML form POSTs:
  - setting status to Active / Safe / Away
  - triggering actuator recalibration
  - extending to an arbitrary percentage
  - setting the wind upper/lower thresholds and dwell times
  - setting the current time (HH:MM)
  - setting the full three-point schedule
- **FR-30** `GET /recalibrate` shall trigger calibration directly.
- **FR-31** The page shall be legible on a phone held at arm's length in sunlight —
  hence the `5vw`–`10vw` font sizing throughout.

### 3.8 Cloud service

| Method | Path | Behaviour |
| --- | --- | --- |
| GET | `/` | Health check, returns `Hello World!` |
| GET | `/time/est` | `{ datetime }` — now, in America/New_York, as ISO |
| GET | `/update/check/:version` | `{ updateAvailable: boolean }` — does the image exist |
| GET | `/update/download/update-v:version.bin` | Streams the firmware image from `server/updates/` |
| POST | `/data` | Writes a telemetry document to MongoDB |

- **FR-32** The service shall listen on `0.0.0.0:443`.
- **FR-33** Firmware images shall be read from `server/updates/` on disk, named
  `update-v{n}.bin`. These are git-ignored.
- **FR-34** Telemetry shall be persisted to the `datalogs` collection with the shape:
  `{ time, temperature, extended_percent, status, wind_speed, button_panel: { settings, wind_sensor, away } }`.
- **FR-35** `MONGODB_URI` shall come from the environment (`.env`, git-ignored).
- **FR-36** The service shall be deployable as a container.

---

## 4. Non-functional requirements

- **NFR-1 — Unattended operation.** The site is remote and off-grid. No path may block
  forever waiting for a human, and no failure short of hardware death may require a site
  visit.
- **NFR-2 — Power frugality.** The RTC rail is switched, the radio is off unless needed,
  and the settings AP only runs when a technician physically enables it.
- **NFR-3 — Degrade, don't stop.** Wi-Fi down → keep tracking on the RTC. Wind sensor
  disabled → keep tracking. Calibration lost → re-learn it.
- **NFR-4 — Remote serviceability.** OTA is the primary support channel.
- **NFR-5 — Memory.** ATmega-class RAM discipline: `StaticJsonDocument<200>`, `F()` macros
  on constant strings, HTML streamed to the client rather than assembled in a buffer.
- **NFR-6 — Physical override.** Switches, not software flags, decide whether the
  settings AP and the wind sensor are live.

---

## 5. Known defects and gaps

Ordered by consequence. GitHub issue numbers where they exist.

### Must fix

1. **`ClockModule::isAlarmTriggered()` has no `return true`** (`src/Clock/Clock.cpp:48`).
   It returns `false` correctly, but falls off the end of the function when the alarm
   *did* fire — undefined behaviour, and the hourly move depends on it. In practice the
   caller reads whatever `resetAlarm()` left in the return register, which is the byte
   count from its last `Serial.println()` — truthy, so the unit tracks by luck. This is
   decided once at compile time, not per call: for any given binary it either always
   works or always freezes. The risk is at rebuild (core/library upgrade, optimisation
   change, inlining), and the failure is silent.
2. **Wind dwell times are off by 10×** (`SettingsServer.cpp` and `WindSpeedSensor::toHtml`).
   The UI labels the field "minutes" and the code multiplies by `6000`, not `60000`. The
   6000 ms default therefore parks the array after **6 seconds** of gust, not one minute.
   This is issue **#34, "Safe mode triggers too quickly."**
3. **The storm position is derived from the tracking schedule** (issue **#35**).
   On this frame **50% extension is flat**, and flat is the correct stow — wind direction
   is unknown, so the only option is to present as little as possible in every direction.
   `HALF_DAY_PERCENT` is already 50; the bug is that `getHalfDayExtensionPercent()` passes
   it *through* `normalizePercentage()`, the time-of-day → extension interpolator. That
   asks "where would the tracker be halfway through the day", which evaluates to **69%** —
   19 points past flat, ~21° of tilt still facing the wind. Worse, the value tracks the
   schedule: set the dashboard percentages to 30/50/70 and SAFE parks at 55%; 20/40/60 and
   it parks at 45%. Never 50. Fix is a named `STOW_PERCENT = 50` passed straight to
   `extendToPercent()`. That routes through `moveTo()`, not `home()`, so it is
   **independent of defect 4**.
4. **`home()` works exactly once per power cycle** (`LinearActuator.cpp:49`):
   `isHomed == 0;` is a comparison, not an assignment, and `isHomed` is a member that
   nothing else ever clears (`grep isHomed` → 4 hits, one of them the initializer). The
   first call homes and latches the flag to 1; every later call finds
   `while (isHomed == 0)` already false, skips the body, and returns without moving or
   resetting `pos`. Compare `max()` ten lines below, which does `isMaxed = 0;` correctly.

   Scope is narrower than it looks. `home()` is reachable only via `extendToPercent(0)`
   with an exact zero — which never happens, the schedule bottoms out at 20% — or via
   `recalibrate()`. Boot always gets one good homing run, and a power cycle clears the
   latch. The real cost is that every *subsequent* recalibration in the same power cycle
   measures `maxPos` from a position that was never zeroed, so accumulated step drift gets
   baked into the calibration instead of removed by it. Recalibration is the only drift
   recovery in the system. Severity: medium.
5. **`normalizePercentage()` shadows its own variables** (`Clock.cpp:118-121`). Inside the
   morning branch, `int startHour` and `int finishHour` are re-declared instead of assigned,
   so the morning interpolation silently uses the afternoon hours. Morning positions are
   wrong.

6. **The hourly alarm is swallowed whenever status isn't ACTIVE**
   (`SolarTracker.cpp:24`). `if (clockModule.isAlarmTriggered() && status == ACTIVE)` —
   `isAlarmTriggered()` clears the flag and re-arms the alarm as a side effect, and C++
   short-circuits left to right, so it runs before `status == ACTIVE` is tested. Any alarm
   firing in NIGHT, SAFE or AWAY is consumed and discarded. 08:00 is the worst case: it is
   the moment the tracker transitions out of NIGHT, and it is the hour that gates the daily
   sync. Fix by hoisting the call: `bool alarm = clockModule.isAlarmTriggered();`

7. **`moveTo()` leaves the motor energised when it aborts**
   (`LinearActuator.cpp`, inside `moveTo`). On an unexpected boundary it prints and
   `return false` without calling `extend(STOP)`. The caller's next action is
   `recalibrate()`, whose first statement is `delay(5000)` — five seconds of a stalled
   motor driving into a hard stop. `extend()` also only resets `speed = 255` in its STOP
   branch, so the low PWM from the approach taper (minimum `difference + 100` ≈ 40% duty)
   persists into the following `max()`, which may then stall immediately and record a
   garbage `maxPos`.

   **Field symptom (reported by the operator):** early-morning hang with the actuator
   audibly whining, cleared only by a power cycle. Chain: defect 4 computes −2% at 08:00 →
   target is past the home end stop → full-speed drive into the stop → stall → abort
   without stopping → `recalibrate()` grinds for 5 s → `max()` enters
   `while (isMaxed == 0)` whose only exit is `hitBoundary()`, and `hitBoundary()` restarts
   its 1-second timer on *any* change to `steps`. A buzzing stalled motor generates
   spurious hall counts well below the 500 µs debounce, so the loop never exits and the
   motor is never released. Power cycling works because by then it is past 08:00 and every
   later hour targets a reachable positive percentage.

   Minimum fix is `extend(STOP);` before that `return false`. `max()` and `home()` also
   need an iteration or wall-clock timeout so no loop can hold the motor indefinitely.

### Should fix

6. **Telemetry is not wired up.** `WifiClient::post()` is an empty `// TODO`. The server
   endpoint, the Mongoose schema, and the collection all exist and nothing ever writes to
   them. Issue **#12**.
7. **The service speaks plain HTTP on port 443.** Express is not configured with TLS, but
   the firmware forces `https://` and port 443 for both the time sync and the OTA download.
   Deployment must therefore put a TLS terminator in front of the container — that
   requirement is undocumented and unenforced.
8. **`/update/download` takes the version straight from the URL into a path join.** It is
   constrained to the `update-v{version}.bin` pattern, which limits the damage, but the
   parameter is unvalidated.
9. **The settings AP is an open network** with unauthenticated control of the actuator.
   Acceptable if the switch is only flipped by a technician standing at the unit; worth a
   deliberate decision rather than a default.
10. **`/data` opens and closes a Mongo connection per request** (`routes.js:52-56`), and
    closes it before the response is sent. Connect once at boot.
11. **Wi-Fi credentials cannot be changed in the field** — they are compiled in. Issue
    **#36**.
12. **The RTC randomly powers down overnight.** Issue **#19**, still open, with an
    abandoned band-aid branch (`feature/19-bandaid-for-clock-resetting`). The software
    power-cycle on D8 and the 3× retry at boot are the current mitigation.

### Nice to have

13. **No settings persistence.** Every schedule and threshold change is lost on reset.
    Issue **#1** (SD card support) covers this; a branch exists and was never merged.
14. **No LED matrix status display.** Issue **#22** — the R4 WiFi has one built in and it
    is unused.
15. **HTML is hand-concatenated in every class.** Issue **#11**/**#32** proposed a small
    HTML builder.
16. **No build configuration.** No `platformio.ini`, no CI, no `arduino-cli` config, no
    `secrets.h.example`. Building this requires knowing the board and the library list by
    heart.
17. **No tests anywhere**, firmware or service.

---

## 6. Build and run

**Firmware**

1. Create `secrets.h` at the repo root defining `wifiCredentials` and `API`.
2. Install the libraries listed in §2.
3. Compile and flash `solar-tracker.ino` for Arduino UNO R4 WiFi.
4. Serial monitor at 9600 baud.

**Service**

```
cd server
npm install
echo "MONGODB_URI=..." > .env
npm run serve          # or: docker build -t solar-tracker . && docker run -p 443:443 solar-tracker
```

**Publishing an OTA update**

1. Bump `OTAUpdater::VERSION` in the firmware source.
2. Export the compiled binary as `server/updates/update-v{new_version}.bin`.
3. Devices pick it up at their next 08:00 sync.
