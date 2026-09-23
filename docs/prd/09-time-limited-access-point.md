# 09 — Time-limit the settings access point

**Status:** Proposed
**Owner:** unassigned
**Depends on:** nothing
**Blocks:** 02, 05, 06 — every one of them promises "the unit reports in within N minutes", and none of them can promise anything while the settings switch is on
**Estimate:** 1.5 days. ~6 h firmware (window state machine in `solar-tracker.ino`, a bounded client read in `SettingsServer`, one dashboard line), ~2 h for a bench sketch that exercises three windows in one power session, 24 h unattended soak (not hands-on), ~1 h to flash and verify in the field.
**Cost:** none. No new cloud resource, no server redeploy, no change to the `/data` contract.

**Do this first, before 02, 05 or 06.** It is the cheapest PRD in the set and it is the precondition for every reliability claim the others want to make. Today a switch left in the wrong position turns the tracker into a device that cannot be reached, cannot be diagnosed, and cannot be updated, for as long as nobody drives out to flip it back.

## Problem

`CWifi::beginAP()` writes `AT+WIFIMODE=2` and `CWifi::begin()` writes `AT+WIFIMODE=1`; WiFiS3 never writes mode 3, so the radio is an access point or a station and never both — the firmware's own comment says so at `solar-tracker.ino:85-90`. While D7 reads HIGH the tracker therefore ships no telemetry (`SolarTracker::uploadLogs()` returns early on `appMode`, `SolarTracker.cpp:278-282`), queues no sync (`serviceSync()`, `SolarTracker.cpp:136`), syncs no clock, and could not accept an OTA image if one existed. The unit was left with that switch ON at the end of the 2026-09-09 visit, which is why no 08:00 sync appeared the next morning; the retained Cloud Run request log holds **20 requests total across 2026-08-21 to 2026-09-09, all `/time/est`, in four bursts on three days, then nothing at all for the 16 days from 2026-08-24 14:34 EDT onward** (`docs/diagnostics/cloudrun-requests-2026-09-09.tsv`). From the cloud side, "switch left on" and "board is dead" look identical, which is the part that costs real money: it is indistinguishable-silence that puts someone in a truck.

## Goal

The access point serves for at most 15 minutes per deliberate switch toggle, after which the radio returns to station mode on its own, whatever position the switch is left in.

## Non-goals

- Reaching AP+STA concurrency, patching WiFiS3, or writing `AT+WIFIMODE=3` from the sketch. Discussed under Design as the rejected long-term alternative; not built here.
- Rewriting `SettingsServer`'s HTTP handling, dashboard markup, or settings schema. The only changes are a bounded read deadline and one status line.
- Any change to the 8-column CSV row, `Telemetry`, the spool, or `server/routes.js`. Nothing is redeployed.
- A watchdog. This PRD reduces the ways the unit can go silent; it does not add a backstop for the ones that remain. That is PRD 02's job.
- Fixing the D7 wiring fault. That is physical (`docs/wiring.md`, "Gotchas": wire the spare SPDT throw to 5 V through 10 k).
- Changing sync cadence, telemetry interval, or the wedge-count-in-`wind_peak` arrangement.

## Requirements

1. Close the access point 15 minutes after its listening socket opens, regardless of the D7 position at that moment.
2. Arm the window from the instant `SettingsServer::start()` returns true — not from `setup()` entry and not from the switch transition — so a slow boot or a retried socket does not eat the window.
3. Arm a window at boot when D7 already reads HIGH, so a switch left on after a site visit self-heals within 15 minutes instead of never.
4. Return the tracker to station mode on window close by clearing `appMode` and queueing one sync, so the spool drains at the next gated opportunity rather than waiting up to 30 minutes for `MONITOR_SYNC_MS`.
5. Require a debounced OFF→ON transition of D7, with at least 10 s of continuous OFF, before a new window arms.
6. Refuse to re-open the access point while D7 stays HIGH after a window has closed.
7. Run the window-close transition only when `tracker.actuator.status == LinearActuator::STOP`, because the close path issues `AT+SOFTRESETWIFI` and any single AT command can stall the RA4M1 for the full `MODEM_TIMEOUT` of 10 s (unverified: `MODEM_TIMEOUT`/`buf_read` read from core source in research claim 2.7, not timed on this unit).
8. Gate `SettingsServer::start()` on the same stopped-actuator test, because a failed AP bring-up burns the full `AP_START_TIMEOUT` — measured at 20,420–20,450 ms per attempt in `docs/diagnostics/ap-cycle-test-2026-09-10.log` — inside a `loop()` that is re-entered with the motor energised.
9. Bound `SettingsServer::checkForClient()`'s `while (client.connected())` loop (`SettingsServer.cpp:109`) with a deadline of no more than 5 s, because that loop has no timeout today and a connected client that stops sending pins `loop()` and defers the window close indefinitely.
10. Show the remaining window on the dashboard, counting down in the browser with no network round trip.
11. Print every state change to serial with the remaining seconds, in a form that greps.
12. Keep telemetry column 6 as exactly `0` or `1`, redefined from "switch position" to "AP window open" — `parseRow()` rejects any row whose `settings`/`wind_sensor`/`away` field is not the literal string `"0"` or `"1"` (`server/routes.js:59-60` on `feature/telemetry-api`), so a minutes-remaining integer in that column would silently drop the whole row.
13. Keep spooling rows during the window exactly as today, so a window costs delivery latency and never data.
14. Keep `WiFi.end()` immediately before every `beginAP()` (`solar-tracker.ino:145`).
15. Prove three consecutive windows in one power session on the bench, with no power cycle, before flashing the field unit.
16. Leave the SD card and the RTC untouched: no file the firmware did not create is deleted, and no `setDateTime()` call is added.

## Design

### State machine

Four variables in `solar-tracker.ino`, next to the existing debounce state. No new class, no heap.

```cpp
const unsigned long AP_WINDOW_MS   = 900000UL;  // 15 minutes
const unsigned long AP_REARM_OFF_MS = 10000UL;  // D7 must read OFF this long to re-arm

bool apWindowOpen     = false;   // listening socket is up and the clock is running
bool apWindowConsumed = false;   // this switch-on has had its window; needs OFF then ON
unsigned long apWindowStarted = 0;
unsigned long switchOffSince  = 0;
```

Transitions, all driven from `loop()`:

| From | Trigger | Action |
|---|---|---|
| closed, D7 OFF→ON debounced, OFF held ≥ 10 s | `onSettingsSwitchChanged(true)` | `server.stop(); WiFi.end(); server.start()` — unchanged — then on success `apWindowOpen = true; apWindowStarted = millis(); apWindowConsumed = false;` |
| open | `millis() - apWindowStarted > AP_WINDOW_MS` **and** actuator stopped | `closeApWindow()` |
| open, D7 ON→OFF | existing switch-off path | `closeApWindow()` early |
| closed, D7 still ON | any | nothing — `apWindowConsumed` blocks re-open |

```cpp
void closeApWindow()
{
  Serial.println("AP window closed, returning to station mode");
  server.stop();          // WiFiServer::end(), resets _sock to -1
  WiFi.end();             // AT+SOFTRESETWIFI: drops the beacon so the SSID visibly disappears
  apWindowOpen = false;
  apWindowConsumed = true;
  tracker.setAppMode(false);   // also queues one sync, see below
}
```

`setAppMode` gains two lines so leaving app mode asks for a sync instead of waiting on the 30-minute fallback:

```cpp
void SolarTracker::setAppMode(bool isAppMode)
{
  if (appMode && !isAppMode) syncPending = true;  // serviceSync() still holds the gate
  appMode = isAppMode;
}
```

That is the whole fail-safe argument: the close transition does two cheap calls and sets a flag. The expensive part — the station association, the clock GET, the chunk POSTs — happens inside `serviceSync()`'s existing gate (`status != SAFE && actuator.status == STOP`, `SolarTracker.cpp:140`), which is already the only place in this firmware that is allowed to block.

`WiFi.end()` at close, rather than letting `ensureWifiConnection()` do it at the next sync (`WifiClient.cpp:26`), is deliberate: without it the radio sits in `WIFIMODE=2` beaconing `Solar-Tracker` with no listening socket, and a technician sees an SSID that joins and then serves nothing. Dropping the beacon makes the state legible from a phone.

### Why one bring-up per toggle is safe

The AP path leaks listening sockets. Phase A of the 2026-09-10 AP-cycle test — nothing but `beginAP`, `server.begin`, `server.end` in a loop — **first failed at iteration 5**, and every later attempt burned 20.4 s returning `sock=-1`. Phase C, which adds `WiFi.end()` before each `beginAP()`, went **12 for 12 at `sock=0`, 25–29 ms each, recovering a radio that phase A had already exhausted** (`docs/diagnostics/ap-cycle-test-2026-09-10.log`; commit 48411ae). The sketch already does `WiFi.end()` first at `solar-tracker.ino:145`, so the mitigation is in place and measured on this hardware. Requirement 5's 10 s OFF guard keeps switch chatter from consuming bring-ups; requirement 14 keeps the leak closed.

This does reverse commit 48411ae's conclusion ("the tracker stops cycling it… the access point comes up once at boot and is never taken down"). That conclusion was reached when the alternative was cycling the AP *around every sync* — dropping the dashboard for 10–15 s, repeatedly, while somebody was standing there using it. One close per deliberate toggle is a different trade: the dashboard is never interrupted mid-session, and the same commit already recorded that cycling is possible.

### Where the 15 minutes shows up

| Option | What it costs | Verdict |
|---|---|---|
| **A. Column 6 changes meaning: "AP window open" instead of "switch position"** | Firmware only, zero server change, zero risk to the backlog. Resolution is the 10-minute `TELEMETRY_INTERVAL`, so a 15-minute window shows as 1–2 rows with `settings=1`. Raw switch position is no longer reported. | **Recommended** |
| B. Add a 9th column carrying seconds remaining | Exact, but `parseRow()` returns `null` unless `parts.length === 8` (`routes.js:47`), so the moment the server moves to 9 fields every 8-column row already on the card — up to 64 chunks, about 50 days — is silently dropped with a 200. A schema bump is only safe once `bench/00_card_inventory` shows the spool empty. | Later, if ever |
| C. Multiplex into `wind_peak` beside the wedge count | A third meaning on one field, selected by `ButtonPanel::isWindSensorEnabled()`, which floats HIGH. | Reject |

Option A is enough for the thing that matters. Today a switch left on produces *no rows at all*; after this change the cloud shows one or two `settings=1` rows followed by `settings=0` rows, all delivered on the next sync, and "switch left on" stops looking like "board is dead". The minutes-remaining number lives where a human can act on it: the serial log and the dashboard.

Dashboard, in `SettingsServer::sendDashboardTo()` — a number plus ~120 bytes of inline script, no network:

```cpp
client.print(R"(<p id="apw" style="font-size:5vw;">)");
client.print(R"(Access point closes in <span id="apr"></span>.
  Toggle the switch off and on for another 15 minutes.</p>
  <script>var s=)");
client.print(apSecondsRemaining());
client.print(R"(;setInterval(function(){s=s>0?s-1:0;
  document.getElementById('apr').textContent=Math.floor(s/60)+'m '+(s%60)+'s';},1000);</script>)");
```

Serial, greppable: `AP window open, 900s remaining`, `AP window closed, returning to station mode`, `AP re-arm refused, switch has not been off for 10s`.

### The long-term alternative, and why not now

Research claim 4.8 (high confidence, **adversarial verification did not complete**) says the ESP32 bridge firmware *does* implement `WIFI_MODE_APSTA` at `cmds_wifi_station.h:225` — the silicon and the bridge can do AP and station at once, and only WiFiS3 refuses to ask. This contradicts how the constraint is written down in this repo: `solar-tracker.ino:85` ("The radio can only do one job at a time"), `docs/ota-esp32-bridge.md:126`, and the `ap-switch-blocks-all-syncing` note all describe it as a property of the radio. It is a property of a sketch-side library, which is the kind of thing that can be worked around without touching the bridge at all.

It is still not the move today. It is unverified on this unit's bridge firmware, which is below the 0.6.0 the core expects (every boot prints "Please upgrade the firmware"); the AP listening-socket leak is orthogonal and would remain; and concurrent AP+STA does nothing about the real failure mode, which is an access point left serving to nobody for 16 days. Time-limiting the window is correct under AP+STA too. Log the APSTA path as a separate experiment, and note the cheap first test: `ModemClass modem` is a global in the core, so writing `WIFIMODE=3` may be reachable from the sketch without forking anything — worth 30 minutes on the bench, not worth a dependency.

### Budget

Expect under 1 KB of flash and under 32 bytes of globals — four variables plus strings. Confirm with `arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi solar-tracker.ino` against the 127,688 / 11,212 baseline. Note that research claim 3.17 corrects the denominator: usable application flash is **245,760 bytes** (0x40000 minus the 0x4000 bootloader offset), not the 262,144 `boards.txt` advertises, so the true figure is 52.0% used, not 48%. Either way this PRD is noise against it.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| A bug in the new state machine leaves the radio in neither mode — no AP, no station — and the unit is unreachable locally *and* remotely. **Makes a site visit certain, not just likely.** | low, but this is the one to fear | total loss of both control paths until someone drives out | every transition is `server.stop()` / `WiFi.end()` / flag writes only, no new blocking call; three-window bench soak before flashing (req 15); land PRD 02's watchdog as the backstop |
| The window closes while a technician is mid-configuration | medium | they lose the dashboard and have to toggle the switch and wait 10 s | countdown is on the page (req 10); 15 min is generous for the actual tasks the dashboard does; they are already on site, so this never causes a visit |
| D7 drifts — the serial capture of 2026-09-10 reads `Settings Server: 1` in 189 of 490 status blocks and `0` in 301, and commit 71b63aa records the pin "sits in a wrong position for minutes at a time" | high, it is happening now | a minute-scale OFF then ON re-arms the window and steals up to 15 min of station time per flap | the 10 s guard stops bounce chatter but not minute-scale drift; the change is still a strict improvement, because worst case goes from *unbounded* AP to 15 min per flap; the real fix is the 10 k pull in `docs/wiring.md` |
| A stalled AP client pins `checkForClient()` and the window never closes | medium — no timeout exists on that loop today | the exact failure this PRD is meant to remove, reintroduced | requirement 9's 5 s read deadline is not optional; test it with `nc` holding a socket open and sending nothing |
| The close transition's `AT+SOFTRESETWIFI` stalls 10 s with the motor energised | low once gated | panel overruns its target | requirement 7 gates the close on `actuator.status == STOP`; overrunning the 15-minute window by the length of a move is the correct trade |
| Repeated bring-ups exhaust AP listening sockets again | low | dashboard gone until a power cycle — **site visit** | `WiFi.end()` before every `beginAP()` (req 14), measured 12/12 recovery in phase C; one bring-up per human toggle |
| Column 6 changes meaning and old dashboards, notebooks or queries silently misread it | medium | confusion, not data loss | the rename is the point; record it in `SPEC.md` and in the field name comment at `routes.js:27-37` even though the wire format does not change |
| Continuous station-mode uptime where there used to be none confounds the open DS3231 wedge diagnosis | medium | the motor-current hypothesis gets harder to isolate | research claim 2.16 already found the RF-aggravation theory contradicted by the 2026-09-18 bench run; still, flash this alone, not stacked with 05 or 06 |

## Acceptance criteria

- [ ] Bench: three consecutive windows in one power session, no power cycle — serial shows three `AP window open` lines each followed by an `AP window closed` line at 900 ± 5 s, and a `sock=0` bring-up each time.
- [ ] Bench: with D7 held HIGH continuously, exactly one window opens and the AP never returns; serial shows the re-arm refusal.
- [ ] Bench: D7 toggled OFF→ON inside 10 s does not arm a window; toggled OFF for 11 s then ON does.
- [ ] Bench: a TCP client that connects to port 80 and sends nothing is dropped within 5 s, and the window still closes on time with that client attached.
- [ ] Bench: a window expiring during an actuator move closes only after `actuator.status == STOP`, and the commanded position at the end of that move is within the normal tolerance — no overrun attributable to the close.
- [ ] Field: with the switch left ON, `gs://solar-tracker-442903-telemetry/raw/` receives rows within 30 minutes of the switch going on, and those rows show 1–2 consecutive `settings=1` entries followed by `settings=0`.
- [ ] Field: the `Solar-Tracker` SSID is no longer visible from a phone 16 minutes after the switch goes on.
- [ ] Field: a clock sync appears in the Cloud Run request log within 30 minutes of a window closing.
- [ ] `arduino-cli compile` reports flash ≤ 128,700 bytes and globals ≤ 11,260 bytes.
- [ ] `bench/00_card_inventory` before and after shows no telemetry chunk deleted that was not uploaded, and the RTC still holds correct time across the change.

## Open questions

- **The premise is load-bearing and unverified on this unit.** That WiFiS3 never writes `WIFIMODE=3` rests on research claim 4.8 reading core 1.6.0 source (`WiFi.cpp:49` and `:95`) plus the firmware's own comment at `solar-tracker.ino:85-90`; the adversarial verification pass did not run. Two independent sources agree, but nobody has watched the AT traffic on this board.
- Is 15 minutes right? It should be long enough for a recalibration plus a schedule change on a phone browser. Nobody has timed a real configuration session. If it is closer to 25 minutes, the constant changes and nothing else does.
- Should the dashboard get an explicit "extend 15 minutes" button? Rejected here to honour "toggling the switch is the only way to get another window", but the argument against is thin — a POST is as deliberate as a toggle, and a technician who is cut off mid-task will be annoyed. Decide before building, not after.
- Is `modem.write(PROMPT(_WIFIMODE), "3")` reachable from the sketch, and does the bridge at this unit's firmware version accept it? Thirty minutes on the bench would settle whether AP+STA is a real option, which would change the long-term shape of this whole area even though it does not change what gets built now.
- Does anything downstream read `settings` as "switch position"? Nothing in the repo does, but a BigQuery view or a notebook outside it might.
- Should the window also be bounded per power session, not just per toggle, as insurance against the D7 drift fault? A hard cap protects station time but can leave a technician with no dashboard until a power cycle. Leaning no; wants a decision.
