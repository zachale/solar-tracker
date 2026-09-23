# 08 — Audit the button panel switch pins

**Status:** Proposed
**Owner:** unassigned
**Depends on:** nothing
**Blocks:** nothing
**Estimate:** ~5 hours hands-on (0.5 h metering, 0.5 h PCR probe, 2 h `ButtonPanel` rewrite, 1 h bench sketch, 0.5 h unit test, 0.5 h updating `docs/wiring.md`), plus a 24 h unattended soak
**Cost:** none — two 10 kΩ resistors from stock in the worst case

**Do first:** put a meter on D7, D4 and A2 in both switch positions and write the six numbers down. The board is on the bench at the home PC right now. Everything below is guesswork until those six numbers exist, and after the unit goes back to the field they cost a site visit instead of thirty minutes.

## Problem

Three panel switches decide whether the settings AP runs, whether the wind sensor is live, and whether the array parks in away mode, and all three are read as a bare `digitalRead(pin) == HIGH` (`src/ButtonPanel/ButtonPanel.cpp:12,17,22`) with no evidence that both switch positions are electrically defined. Across the two committed serial captures — 383 status blocks in `docs/diagnostics/serial-2026-09-09.log` and 490 in `docs/diagnostics/serial-2026-09-10.log`, 873 samples in total — D4 read `1` every single time and A2 read `0` every single time, while D7 toggled seven times inside the 2026-09-10 capture. A cloud row on 2026-09-20 reported `wind_sensor:true` while Zach had that switch physically off. The consequence is worse than a wrong column: D4 selects whether `wind_peak` carries wind data or the I2C wedge count (`src/SolarTracker/SolarTracker.cpp:154-156`), so the channel currently carrying the DS3231 story is gated on a pin nobody has metered, and A2 is read every `loop()` iteration and drives a real actuator move on every accepted edge (`SolarTracker.cpp:26-33` → `setStatus(AWAY)` → `actOnStatus` → `extendActuatorToSafe()` at `:243-246`).

## Goal

Every panel switch has a measured, documented electrical state in both positions, a debounced read, and a fail-safe value the firmware reports when its configuration cannot be trusted.

## Non-goals

- Does not move the wedge counter out of `wind_peak` — that is PRD 07. This PRD makes the existing selector trustworthy; 07 removes the need for a selector at all. Neither waits on the other.
- Does not change the server's 8-column CSV contract or the Cloud Run service.
- Does not change what any switch *means*. Same three features, same semantics, same dashboard.
- Does not add a watchdog, and does not touch D2 (hall) or D3 (RTC `INT/SQW`) — those are not panel switches and D3's pull-up is doing a different job.
- Does not redesign the panel or replace the switch hardware. At most two resistors and, if the metering demands it, one wire moved from 5 V to GND.
- Does not attempt to explain the DS3231 wedge. Unrelated fault, different PRD.

## Requirements

1. Measure each of D7, D4 and A2 to GND, in both switch positions, with the board powered and the sketch running, and record all six voltages with the date in `docs/wiring.md`.
2. Classify each switch against the four wiring cases in the Design table, and state in `docs/wiring.md` which case it is.
3. Print one boot line reporting, per switch, the Arduino pin, the RA4M1 port pin, the `PCR` (pull-up control) bit read back from `PmnPFS`, the compiled active level, and the raw level — so a wrong build is visible in the first second of serial rather than after a day of rows.
4. Define the active level for each switch exactly once, in a table in `ButtonPanel`, derived from requirement 1 and not from a guess.
5. Debounce all three switches with the same 500 ms window already proven on D7, and sample each pin exactly once per `loop()` iteration so that no single telemetry row can disagree with itself.
6. Report a per-switch fail-safe value whenever that pin's `PCR` readback contradicts the configuration the firmware asked for: settings AP → off, wind sensor → enabled, away override → not away.
7. Latch a switch to its fail-safe value, with one log line, after more than 30 accepted transitions inside 10 minutes on that pin, and clear the latch only on reboot.
8. Delete the duplicated settings-switch debounce from `solar-tracker.ino:12-20,114-130` once `ButtonPanel` owns debouncing — no wrapper, no second code path.
9. Add `bench/14_panel_pins` that prints raw level, debounced level, `PCR` and a transition count per switch, drives D5 and D6 low at boot, and includes no `LinearActuator` — per the bench safety rules in `bench/README.md`.
10. Add a host-side unit test for the debouncer in `test/` with injected time and injected pin level, covering a clean edge, a three-bounce edge inside one second, and the transition-storm latch.
11. Fit physical 10 kΩ pull-downs and revert those pins to plain `INPUT` only if requirement 2 classifies a switch as Case B *and* its ON throw cannot be moved to GND.
12. Record whether `P001` (A2) actually implements an internal pull-up, because the firmware currently assumes it does and cannot tell (unverified: no adversarial verification pass ran, and the RA4M1 hardware manual has not been checked for `PCR` availability on the analog-capable port 0 pins).

## Design

### What is actually wired, as far as the tree can say

| Switch | Arduino pin | RA4M1 port pin | Configured | Read as | Observed in 873 samples |
| --- | --- | --- | --- | --- | --- |
| Settings AP | D7 | `P112` (`variant.cpp:34`) | `INPUT_PULLUP` (`ButtonPanel.cpp:5`) | `HIGH` = enabled | toggles — 7 transitions on 2026-09-10 |
| Wind sensor | D4 | `P106` (`variant.cpp:31`) | `INPUT_PULLUP` (`ButtonPanel.cpp:6`) | `HIGH` = enabled | `1` in 873/873 |
| Away override | A2 = pin 16 (`pins_arduino.h:19`) | `P001` (`variant.cpp:43`) | `INPUT_PULLUP` (`ButtonPanel.cpp:7`) | `HIGH` = away | `0` in 873/873 |

**The received framing of this bug is wrong, and that changes the fix.** The premise going in was "a plain switch to 5 V with no pull-down leaves an open, floating pin". But `ButtonPanel::setup()` has set `INPUT_PULLUP` on all three pins since commit `d19062e`, and an `INPUT_PULLUP` pin that is open is not floating — it reads a deterministic `HIGH`. The likelier failure is the opposite shape: a pin that can never read `LOW`. If a switch closes the pin to **5 V**, then both positions read `HIGH`, the switch is electrically inert, and no firmware change fixes it. That matches D4's 873/873 exactly. So metering comes first, and "add `INPUT_PULLUP`" is not the deliverable — it is already there.

### Four cases, and what the meter says

| Case | Wiring | Meter, position 1 | Meter, position 2 | Verdict |
| --- | --- | --- | --- | --- |
| A | Pin ↔ GND, 2-wire | ~0 V | ~5 V (internal pull-up) | Works. Active level is LOW. Matches `wiring.md`'s "Open = enabled". |
| B | Pin ↔ 5 V, 2-wire | ~5 V | ~5 V | Switch is inert. Predicted for D4. Needs a wire moved or a pull-down. |
| C | SPDT, common to pin, one throw GND, other 5 V | ~0 V | ~5 V, stiff | Best case. Both positions driven. |
| D | Open in one position with no live internal pull-up | ~0 V | indeterminate, drifts | Only possible if `PCR` is not implemented on that pin. Candidate for A2. |

Case A and Case C give the same two voltages; tell them apart by measuring pin-to-5 V, or by whether the reading moves when a finger is near the pin.

**`docs/wiring.md` contradicts itself here, and only one of its two statements can be on the bench.** Its pin table says D4, D7 and A2 are each "Open = enabled", which describes Case A. Its Gotchas section says to wire the SPDT "common to the pin, one throw to ground, spare throw to 5 V through 10 k", which is Case C and has nothing open in either position. Resolving that is the substance of requirement 2. Separately, `SPEC.md` §2's pin table omits A2 altogether, and §3.4 claims `AWAY` is "Set manually from the dashboard" — the firmware sets it from A2 on every `loop()` iteration and nothing in `SettingsServer` sets `AWAY` at all.

### Proving the pull-up is real, not assumed

The core's `pinMode()` calls `R_IOPORT_PinCfg(..., IOPORT_CFG_PORT_DIRECTION_INPUT | IOPORT_CFG_PULLUP_ENABLE)` and **throws the `fsp_err_t` away** (`cores/arduino/digital.cpp:9-11`), so a pin that does not implement the pull-up configures silently and reads like a plain input. Read the bit back instead:

```cpp
// PmnPFS bit 4 is PCR, "Pull-up Control" —
// variants/UNOWIFIR4/includes/ra/fsp/src/bsp/cmsis/Device/RENESAS/Include/R7FA4M1AB.h:713
static bool pullUpLive(uint8_t port, uint8_t portPin)
{
  return R_PFS->PORT[port].PIN[portPin].PmnPFS_b.PCR != 0;
}
```

Reading `PmnPFS` needs no `PWPR` unlock; only writes do. Expect `pcr=1` on `P112` and `P106`. `P001` is the one to watch: if it reads `0`, the away switch has been a bare floating input the whole time and Case D is live.

### Options

| Option | What it takes | Verdict |
| --- | --- | --- |
| `INPUT_PULLUP` + active-LOW, ON throw to GND | Firmware table plus, at most, one wire moved from 5 V to GND | **Recommended.** No new parts, and D7 already demonstrates it works on this board. |
| Plain `INPUT` + external 10 kΩ pull-down, active-HIGH | One resistor per pin inside the panel; enclosure open | Fallback for any pin whose ON throw genuinely cannot reach GND. Note `INPUT_PULLDOWN` is *unavailable on this core* — `digital.cpp:6` maps it to plain `INPUT` — so this is a soldering job, not a code change. |
| SPDT with both throws driven (Case C) | Rewire all three to 3-terminal | Electrically the best answer and what `wiring.md` already recommends, but it is the most bench time for the least marginal gain over option 1. Do it only if the panel is already open. |

### The firmware change

```cpp
// ButtonPanel.h
class ButtonPanel
{
public:
  enum Switch : uint8_t { SETTINGS = 0, WIND = 1, AWAY = 2, COUNT = 3 };

  static void setup();
  static void service();          // sample + debounce; called once, first thing in loop()
  static void printConfig();      // the boot line from requirement 3

  static bool settingsServerEnabled();   // cached, debounced
  static bool isWindSensorEnabled();
  static bool isAwayModeOverride();

private:
  struct Config
  {
    uint8_t arduinoPin;
    uint8_t port;       // for the PCR readback
    uint8_t portPin;
    bool    activeLow;  // set from the measurement in requirement 1 — do not guess
    bool    failSafe;   // reported when PCR contradicts the request, or after a storm latch
  };
  static const Config CONFIG[COUNT];
  static const unsigned long DEBOUNCE_MS = 500;
};

// ButtonPanel.cpp — activeLow values below are PLACEHOLDERS until metered
const ButtonPanel::Config ButtonPanel::CONFIG[COUNT] = {
  {  7, 1, 12, true,  false },  // D7  P112  settings AP    — fail safe: field mode
  {  4, 1,  6, true,  true  },  // D4  P106  wind sensor    — fail safe: enabled
  { 16, 0,  1, true,  false },  // A2  P001  away override   — fail safe: not away
};

void ButtonPanel::service()
{
  unsigned long now = millis();
  for (uint8_t i = 0; i < COUNT; i++)
  {
    if (latched[i]) continue;
    bool level    = digitalRead(CONFIG[i].arduinoPin) == HIGH;
    bool asserted = CONFIG[i].activeLow ? !level : level;
    if (asserted != candidate[i]) { candidate[i] = asserted; since[i] = now; continue; }
    if (asserted != stable[i] && now - since[i] > DEBOUNCE_MS)
    {
      stable[i] = asserted;
      transitions[i]++;
      Serial.println("PANEL sw=" + String(i) + " -> " + String(asserted ? 1 : 0));
    }
  }
}
```

`setup()` seeds `stable[]` and `candidate[]` from one real read of each pin, exactly as `solar-tracker.ino:67-68` does today, so boot behaviour does not change and the settings switch is still known before `tracker.setup()` runs its first sync. `failSafe` overrides that seed only when `pullUpLive()` disagrees with the configuration, and overrides the runtime value only after the requirement 7 latch. Be honest about the limit: a single pin cannot detect its own float at runtime. The debounce defends against bounce, the transition-storm latch defends against sustained flapping, and `PCR` covers a misconfiguration — nothing covers a genuinely noisy input that happens to settle.

**Fail-safe direction, per switch:**

| Switch | Indeterminate must mean | Why that direction |
| --- | --- | --- |
| Settings AP (D7) | off — field mode | `beginAP()` writes `AT+WIFIMODE=2` and `WiFiS3` never selects AP+STA, so a stuck-on D7 silently ships nothing, forever, and the only fix is a site visit (unverified: research dimension 4, `renesas 1.6.0 WiFi.cpp:49` and `:95`, load-bearing and never adversarially checked — though `solar-tracker.ino:85-91` states the same thing from Zach's own testing). |
| Wind sensor (D4) | enabled | Disabled reports 0 m/s and never triggers `SAFE` (`SPEC.md` FR-15, `WindSpeedSensor.cpp:29-35`). A stuck-off D4 leaves the array flat in a gale. |
| Away override (A2) | not away | `setStatus(AWAY)` calls `extendActuatorToSafe()` immediately (`SolarTracker.cpp:243-246`). A stuck or flapping A2 moves the motor with nobody asking. |

The wind-sensor fail-safe and the wedge-count channel are in **direct conflict**: "enabled" is the safe direction and also the value that hides the wedge count. That conflict is unresolvable inside the 8-column contract, and it is the strongest argument for PRD 07 giving diagnostics their own field.

### One sample per row

`spoolTelemetryRow()` reads D4 twice for the same row — `SolarTracker.cpp:154` for the `wind_peak` selector and `:158` for the `wind_sensor` column. If the pin changes between those two lines, the row ships wind data while claiming the sensor was off, or ships a wedge count while claiming it was on. Caching in `service()` makes that impossible by construction. The same iteration also reads A2 twice (`:26` and `:30`), all three pins in `getButtonStatus()` (`:90`), D7 in `readSettingsSwitch()` (`solar-tracker.ino:118`), and D4 again through `WindSpeedSensor::isEnabled()` on every `getSpeed()`.

`service()` must be the first statement in `loop()`, before `tracker.pollSensorData()`. It is `millis()`-based and never blocks, so it is safe on the re-entrant path from inside the actuator drive loop; an away-switch flip mid-move retargets rather than recursing, which `extendToPercent()` already handles at `LinearActuator.cpp:139-144`.

Budget: roughly 36 bytes of RAM for the state arrays against 21,556 free, and expect well under +1 KB of flash on 127,688 — measure the compile before and after rather than trusting that. While the file is open, `getButtonStatus()` (`ButtonPanel.cpp:27`) builds its line by `String` concatenation every 10 seconds; `snprintf` into a fixed buffer is a free win here, but it is not required by this PRD.

## Risks

| Risk | Likelihood | Impact | Mitigation |
| --- | --- | --- | --- |
| Deferring the metering until the unit is back in the field turns a 30-minute bench job into a site visit | high if not done now | medium | Do it while the board is on the home PC. This is the whole reason to schedule this PRD early: right now it costs no trip. |
| The probe slips and shorts a panel pin to GND or 5 V, killing the pin | low | high — a dead D7 or D4 is a board swap, i.e. a site visit | Meter in DC volts, never ohms or current, one hand, black lead clipped to the star ground first, actuator supply disconnected. |
| The active level is inverted in the build and the tracker ships permanently in AP mode | low | high — zero telemetry until someone walks up to it | Requirement 3's boot line names the compiled active level and the raw read; requirement 9's bench sketch exercises both positions of all three switches before anything is flashed. |
| Metering finds nothing wrong and the 2026-09-20 row had another cause | medium | low | Rows sit on the card until a sync, so a row spooled before the flip and uploaded after would look identical. Compare that row's unix timestamp against when the switch was flipped — the data is on the home PC, not the Mac. The 873/873 D4 evidence is independent of this confound either way. |
| A 500 ms debounce on A2 delays a genuine away request | high | negligible | Half a second before a park that takes tens of seconds. |
| Physical pull-downs get fitted and `docs/wiring.md` is not updated, so the next audit re-derives the same unknowns | medium | low | Requirement 1 and 2 land in `wiring.md` in the same change, dated. |
| The transition-storm latch fires on a technician legitimately toggling the settings switch while debugging | low | low | Threshold is 30 accepted transitions in 10 minutes; they are standing at the unit and can power-cycle it. |

## Acceptance criteria

- [ ] `docs/wiring.md` contains six dated voltage measurements — D7, D4, A2, each in both switch positions — and names the wiring case for each switch.
- [ ] Serial at boot prints one line of the form `PANEL d7=P112 pcr=1 activeLow=1 raw=1 | d4=P106 pcr=1 activeLow=1 raw=1 | a2=P001 pcr=? activeLow=1 raw=0`, and every `pcr` digit is consistent with what the meter implies.
- [ ] On the bench, each of the three switches produces an accepted `PANEL sw=N -> X` line within 1 s of every flip, for at least 10 flips each, with no line in between flips.
- [ ] A 24 h soak with all three switches untouched logs zero `PANEL sw=` transitions and zero latch lines.
- [ ] A cloud row taken with the wind switch physically off has CSV column 7 equal to `0`. It is `1` today.
- [ ] Toggling the wind switch continuously across a telemetry spool still yields a row whose `wind_peak` and `wind_sensor` columns agree about which mode produced them.
- [ ] Off→on on the away switch produces exactly one `BENCH_TEST: would extend to safe N%` line, on→off produces exactly one `would extend to N%`, and nothing in between.
- [ ] `test/run.sh` passes, including the new debounce test with its three cases.
- [ ] `solar-tracker.ino` contains no `settingsSwitchCandidate`, `settingsSwitchSince` or `readSettingsSwitch` — the debounce lives in exactly one place.
- [ ] Compiled size stays within +1 KB flash of 127,688 bytes and +64 B RAM of 11,212 bytes, with both numbers recorded.

## Open questions

- Does `P001` implement `PCR` at all? The firmware has assumed a pull-up on A2 since `d19062e` and cannot tell, because `digital.cpp:9-11` discards `R_IOPORT_PinCfg`'s error. This is load-bearing for the whole away-mode fail-safe argument and needs the RA4M1 hardware manual plus the readback in requirement 3. No adversarial verification pass ran on any of this research.
- What is physically connected to the third terminal of each SPDT? If it is nothing, all three switches are 2-wire and `wiring.md`'s Gotchas section describes an intention rather than the build.
- Which of `wiring.md`'s two mutually exclusive descriptions — "Open = enabled" in the pin table, Case C in the Gotchas — is actually on the bench?
- Should `AWAY` be a latched panel switch at all? It commands a motor move on every edge, `SPEC.md` §3.4 says the dashboard owns it, and no dashboard control exists. Momentary-plus-software-latch would decouple the move from the pin, but that is a behaviour change and out of scope here.
- What was the exact timestamp on the 2026-09-20 row, and when was the switch flipped? Needed to rule out the spool-then-upload confound. The log is on the home PC's checkout.
- Is 30 transitions in 10 minutes the right storm threshold, and should the latch really persist until reboot for D7 specifically?
- Is the internal pull-up strong enough for the cable run to the panel? The RA4M1's is weak — tens of kΩ — and the exact figure should come out of the hardware manual before any external resistor value is chosen.
