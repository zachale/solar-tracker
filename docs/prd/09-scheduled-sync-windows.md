# 09 — Scheduled sync windows

**Status:** Proposed
**Owner:** unassigned
**Depends on:** nothing
**Blocks:** 02, 05, 06 — each of them promises "the unit reports in within N minutes", and today none of them can promise anything at all while the settings switch is on
**Estimate:** 2 days. ~8 h firmware (window scheduler, the `WiFi.end()` ordering fix, a bounded restore path), ~3 h for a bench sketch that runs 20+ cycles in one power session, 24 h unattended soak (not hands-on), ~1 h to flash and verify.
**Cost:** none. No new cloud resource, no server redeploy, no change to the `/data` contract.

Supersedes the earlier "time-limit the AP" framing. A cloud window runs every fifteen minutes in both switch positions. With the switch off that is simply a faster sync and the radio never leaves station mode. With the switch on the access point is the resting state and the radio steps out of it briefly, on schedule, rather than the dashboard and the cloud being mutually exclusive.

## Problem

The radio can only be an access point or a station, never both. `CWifi::beginAP()` writes `AT+WIFIMODE=2`, `CWifi::begin()` writes `AT+WIFIMODE=1`, and WiFiS3 never writes mode 3. The bridge firmware does implement `WIFI_MODE_APSTA` (`cmds_wifi_station.h:225`), so the silicon is capable — the R4 library simply gives no way to ask for it.

The consequence today, from commit `48411ae`: with the settings switch on, telemetry spools to the card and nothing is pushed, the clock never syncs, and no update can ever be fetched. The device was left in exactly that state after the 2026-09-09 visit, which is why no sync appeared on 2026-09-10 and why the Cloud Run logs contain long silences across the unit's whole life.

So the dashboard and the cloud are mutually exclusive, and whichever one was last selected by a physical switch wins indefinitely.

## Goal

The tracker reaches the cloud at least once every 15 minutes regardless of switch position, and the access point is available whenever the switch asks for it.

### Two resting states, one cadence

The 15-minute cadence is the invariant. What the radio does *between* windows depends on the switch, and the two cases are very different in risk:

| Settings switch | Resting state | What a window costs | AP bring-ups per day |
|---|---|---|---|
| **OFF** (normal deployment) | station, idle | nothing — the radio is already a station | **0** |
| **ON** (someone is on site) | access point | one AP teardown and rebuild, 10-15 s | up to 96 |

With the switch off there is no access point, no mode cycling, and no exposure to the bring-up leak described below. A window is just a sync on a 15-minute timer instead of an hourly alarm. That is the state the tracker spends nearly all of its life in, and this PRD barely changes it.

The cycling path exists only while somebody has deliberately turned the dashboard on. That is when the risk lives, and it is bounded by how long a person stands at the device.

## Non-goals

- AP+STA concurrency, or patching WiFiS3 to reach `WIFI_MODE_APSTA`. Out of scope even though the bridge supports it.
- Removing the settings switch or changing what the dashboard does.
- Changing sync content, telemetry format, or the `/data` contract.
- Guaranteeing zero dashboard downtime. Brief, scheduled downtime is the accepted trade.
- The watchdog (PRD 04), though this PRD's failure modes are a strong argument for it.

## Requirements

1. Run a cloud window every 15 minutes on a `millis()` timer, in both switch positions.
2. With the settings switch **off**, create no access point at all and leave the radio in station mode between windows; a window is then a plain sync with no mode change.
3. With the settings switch **on**, keep the access point as the resting state: leave it for each window, associate as a station, do the cloud work, and return to it.
4. Call `WiFi.end()` before every `beginAP()`. See Design — without this the access point dies permanently after four cycles.
5. Bound the station window so a failure cannot hold the radio out of AP mode indefinitely; return to the access point on a timer whether or not the work succeeded.
6. Restore the access point from `loop()`, not from the sync path, so a failed restore is retried within seconds rather than at the next window.
7. Skip a window while a client is actively connected to the dashboard, and take the next one instead.
8. Publish the time until the next window, and the outcome of the last one, to both the dashboard and the telemetry row.
9. Keep the window scheduler on `millis()`, never on the DS3231 alarm, so a wedged bus cannot stop cloud contact.

## Design

### The finding that decides the implementation

Commit `48411ae` recorded a measurement taken on this exact hardware with an isolated sketch:

> The ESP32 accepts four access point bring-ups per power session and then stops handing out listening sockets permanently. `beginAP()` keeps reporting `WL_AP_LISTENING` and `server.begin()` simply never produces a socket, with every later attempt burning the full timeout. Phase A of that test does nothing but `beginAP`, `server.begin` and `server.end` in a loop, so this is a leak in the access point path itself rather than anything to do with switching between station and access point mode. `server.end()` does not give the socket back.

A 15-minute window is 96 cycles a day. Against a four-cycle budget the dashboard would be gone for good within the first hour.

The same commit records the fix:

> For the record, the same test showed `WiFi.end()` before `beginAP()` fixes the leak completely: twelve for twelve, socket 0 reused every time, 25 ms each, recovering from an already exhausted radio. Cycling is therefore possible.

`WiFi.end()` sends `AT+SOFTRESETWIFI`, which tears down every socket on the ESP32 and returns the pool. That is the whole mechanism. **Requirement 4 is not a nicety; it is the difference between this PRD working and bricking the dashboard.**

Cycling was rejected in `48411ae` for a different reason — it drops the dashboard for the 10-15 s a station association and the HTTP round trips take, and not cycling was less code to go wrong while nobody is on site. That trade is now explicitly reversed: scheduled downtime is preferred to indefinite silence.

### Cost of the trade

96 windows a day at 10-15 s each is 16-24 minutes of dashboard downtime per day, or 1.1-1.7%. A person standing at the tracker sees the dashboard drop for about ten seconds, at most once while they are working, and Requirement 7 removes even that whenever they are actually connected.

### Shape

```
  SWITCH OFF (normal deployment — no AP, no cycling, no leak exposure)

        ┌──────────── resting state ────────────┐
        │  station mode, idle, telemetry spools │
        └──────────────────┬────────────────────┘
                           │ every 15 min (millis)
                           ▼
             clock sync · upload spool · MQTT batch · OTA check
                           │
                           └──────────► back to resting state


  SWITCH ON (someone is on site — this is the path with the risk)

        ┌──────────────── resting state ─────────────────┐
        │  AP up, dashboard reachable, telemetry spools  │
        └───────────────────┬────────────────────────────┘
                            │ every 15 min (millis), and no client connected
                            ▼
              WiFi.end()            ← returns the socket pool; REQUIRED
              WiFi.begin(ssid, pw)  ← station mode
                            │
                            ▼
              clock sync · upload spool · MQTT batch · OTA check
                            │  bounded: hard deadline regardless of outcome
                            ▼
              WiFi.end()            ← again, before coming back
              beginAP() + server.begin() until the socket opens
                            │
                            ▼
                     back to resting state
```

### Choices

| | A. Fixed 15-min period (recommended) | B. Adaptive period | C. Window only when work is pending |
|---|---|---|---|
| Predictability | high — next window always known | low | low |
| Worst-case row age | 15 min | varies | 15 min |
| Dashboard downtime/day | 16-24 min | less when idle | less when idle |
| Reachable when idle | yes — always in contact | yes | **no** — a board with nothing to say is unreachable |
| Complexity | one timer | scheduler state | pending-work plumbing across modules |

**Recommendation: A.** The point of this PRD is that the tracker is never out of contact for longer than a known interval. C defeats that: a healthy idle board would stop calling home, which is exactly when a silent failure becomes indistinguishable from silence. B buys little against a 15-minute floor.

### Restore path

`0cd1a30` already fixed the hard part and its lesson holds here: intent must not be read from state. Whether the access point should exist is the switch's business, not `server.isRunning()` — which is false precisely when a previous restore failed, so the one case needing a retry was the one case that could never get one. Restore belongs in `loop()`, throttled to one attempt every ten seconds, with `begin()` retried until the socket opens and a 20-second tolerance (`0cd1a30` measured four cycles reopening in the same second and a fifth still not done after five).

## Risks

All of the AP-specific risks below apply **only while the settings switch is on**. In the normal deployed state the radio never leaves station mode and none of them exist.

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| `WiFi.end()` omitted or reordered during a refactor | medium | **dashboard permanently dead after 4 cycles** | Requirement 4; a bench sketch that runs 20+ cycles in one power session is the acceptance gate |
| Station association hangs, radio never returns to AP | medium | dashboard unreachable until power cycle | Requirement 5 hard deadline; PRD 04's watchdog is the backstop |
| Someone is mid-edit on the dashboard when a window opens | high | lost form input | Requirement 7 skips the window while a client is connected |
| Cycling aggravates the DS3231 wedge | unknown | telemetry gaps | root cause still open; PRD 03's recovery already handles the observed case. Soak with wedge counters visible |
| 96 extra associations/day wear the bridge in ways 4/day did not — **switch-off case: 96 plain station syncs, no AP churn** | unknown | site visit | 24 h soak before flashing; report reconnect counts per PRD 07 |

The dominant project risk applies here more than anywhere, but only in the switch-on case: that path makes the radio do far more mode switching than the shipped firmware, on a bridge with one measured leak. The switch-off case adds no mode switching at all — only a faster sync timer. Neither should be flashed without PRD 04.

## Acceptance criteria

- [ ] A bench sketch performs 20 consecutive AP→station→AP cycles in one power session with the socket reopening every time.
- [ ] Telemetry rows land in `gs://solar-tracker-442903-telemetry/raw/` no more than 15 minutes apart with the settings switch **off** for a continuous 24 hours.
- [ ] The same, with the settings switch **on** for a continuous 24 hours.
- [ ] With the switch off, the serial log shows zero `beginAP()` calls across that 24 hours.
- [ ] The dashboard is reachable before and after each window, verified across at least 50 cycles.
- [ ] A deliberately failed station association returns the radio to AP mode within the Requirement 5 deadline.
- [ ] A client connected to the dashboard causes the window to be skipped, visible in the serial log.
- [ ] The telemetry row reports time-to-next-window and last-window outcome.

## Open questions

- Is 15 minutes the right period? It is Zach's stated figure. The floor is set by how long a window takes (10-15 s) plus whatever MQTT and log upload add under PRD 05 and 06.
- Does the four-cycle leak have a second limit further out? The `WiFi.end()` test ran twelve cycles, not ninety-six. **The 20-cycle bench gate exists to find out, and the 24 h soak to confirm.** (unverified: extrapolating a 12-cycle result to 96/day)
- Does an OTA download fit inside one window, or does it need a longer one? A ~100 KB `.ota` over this bridge is untimed.
- Should a window be forced immediately when the spool passes a size threshold, independent of the timer?
