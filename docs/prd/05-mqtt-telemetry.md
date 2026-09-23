# 05 — MQTT telemetry delivery

**Status:** Proposed
**Owner:** unassigned
**Depends on:** 07 (schema v2 — publish the new row, not the old one), 09 (the sync window this publishes inside), 04 (watchdog). Also a USB site visit to bring the ESP32 bridge firmware to 0.6.0, and a watchdog + bridge-health reset in firmware — both are separate PRDs in this set
**Blocks:** nothing
**Estimate:** decision 1 hour. Recommended path (HTTPS at a 10-minute cadence): 2 hours build + 48 h bench soak. If MQTT is chosen anyway: 3–4 days build + 3–5 days soak ≈ 1.5–2 weeks wall clock, plus one mandatory site visit.
**Cost:** $0.00/month on the recommended path. $0.00/month for MQTT via EMQX Cloud Serverless free tier (43,200 of 1,000,000 session minutes = 4.3%; ~5 MB of 1,024 MB traffic = 0.5%; 4,320 of 1,000,000 rule actions). Self-hosted Mosquitto would be $3.65/month (always-free e2-micro + $0.005/h external IPv4 × 730 h). Cloud Run as the broker would be ~$44.71/month — rejected.

**Recommendation: do not run a persistent MQTT session on this unit. If "live most of the time" is the actual goal, raise the existing HTTPS upload cadence from hourly to every 10 minutes — that is a two-line change, no broker, no new library, and it delivers the same freshness MQTT-batched would.** MQTT is only worth its cost if you want a downlink channel (cloud → device commands), which the firmware does not currently use.

> **Decision update (2026-09-23).** Zach has chosen MQTT, and has chosen PRD 09's
> permanent-access-point design with scheduled 15-minute sync windows. Those two
> decisions together settle this document: the radio is an access point except during a
> short scheduled window, so a persistent MQTT session is not merely inadvisable here, it
> is **impossible** — there is no station-mode connection to hold open. Option C below is
> therefore off the table on architectural grounds rather than on the reliability grounds
> the original analysis gave. **Build Option B**: connect, publish the batch, disconnect,
> inside each window. Worst-case row age becomes 15 minutes, set by PRD 09's period rather
> than by anything in this document. The rest of the analysis stands as written.


## Problem

Telemetry currently reaches the cloud in hourly batches: `SYNC_EVERY_HOURS = 1` (src/SolarTracker/SolarTracker.h:64) with a 30-minute `millis()` fallback (`MONITOR_SYNC_MS = 1800000`, SolarTracker.h:59), so a row spooled at 08:01 can sit on the card until 09:00. Zach asked whether MQTT could make delivery live most of the time, or batched. It can — the memory cost is small and `mqtts` on 8883 genuinely works through the bridge — but four independent findings say an always-on session is the wrong shape for *this* unit, and the freshness win people expect from MQTT is actually a cadence win that the existing HTTPS path already offers for free. The uncomfortable part: GCP has no managed MQTT broker at all, so any MQTT design makes a third-party service a permanent hard dependency in the telemetry path of a device that cannot be fixed remotely.

## Goal

Telemetry rows land in `gs://solar-tracker-442903-telemetry/raw/` within 10 minutes of being recorded, with no new always-on infrastructure in the delivery path.

## Non-goals

- Mutual TLS or client-certificate auth. Impossible on this board (see Design) — do not spend time on it.
- Cloud → device commands, shadow/twin state, or remote config push.
- Removing the SD spool. It stays the durable queue under every option, because the settings switch kills the radio's station mode outright.
- AP+STA concurrency, or patching WiFiS3/the bridge to reach `WIFI_MODE_APSTA`.
- Streaming full serial logs. That is the logging PRD's problem; this one moves the existing 8-column row only.
- OTA, the watchdog, the DS3231 wedge, and the `wind_peak` wedge-count smuggling. All separate.
- Changing the 8-column CSV schema or the Cloud Run `/data` contract.

## Requirements

1. Decide, in writing, whether a downlink channel is wanted before any MQTT firmware is written — that is the only thing MQTT buys over the recommended cadence change.
2. Raise telemetry delivery cadence so the worst-case age of a row in the cloud is ≤ 10 minutes while the settings switch is off.
3. Keep every network call behind the existing not-moving gate: `status != SAFE && actuator.status == LinearActuator::STOP` (src/SolarTracker/SolarTracker.cpp:140).
4. Keep the SD spool authoritative: delete a chunk only on a confirmed acknowledgement, never on a fire-and-forget publish (current behaviour, SolarTracker.cpp:295–302).
5. Measure the actuator drive-loop iteration rate on the bench and convert the per-call blocking budget into a bound on position error before allowing any new network call inside `extensionCallBack()`.
6. Verify, on the bench, that the chosen path survives 48 hours unattended with zero manual power cycles before it is flashed to the field unit.
7. If MQTT is built: connect, publish, and disconnect inside one gated window — do not hold a session open across `loop()` iterations (unverified: rests on bridge-source reading and forum reports, not a test on this unit's firmware).
8. If MQTT is built: publish the raw 8-column CSV row as the payload and forward it verbatim with `content-type: text/csv`, because `/data` parses nothing else (server/routes.js:19).
9. If MQTT is built: set the keepalive to ≥ 300 s and never rely on keepalive for liveness detection during an actuator move.
10. If MQTT is built: cap the broker at its free quota by setting the EMQX spend limit to 0 (unverified: vendor pricing page, not confirmed in the console).
11. Do not add continuous network activity until the two remaining DS3231 bench steps (supply present, then motor moving) have run — otherwise a motor-current fault and a network-load fault become permanently indistinguishable.

## Design

### What actually works, measured

ArduinoMqttClient 0.1.8 over a second `WiFiSSLClient`, wired into the real sketch with `setId`/`setUsernamePassword`/`setKeepAliveInterval`/`connect` and a `beginMessage`/`print`/`endMessage` publish path, compiled at **131,152 bytes flash (50%) / 11,464 bytes globals (34%)** — **+3,424 flash and +252 RAM** over baseline. 130,992 bytes of flash and 21,304 bytes of locals/stack/heap remain. Runtime adds ~1,024 bytes for the second client's heap-allocated `FifoBuffer<uint8_t,1024>` (WiFiClient.h:253) and ~256 bytes for the MQTT tx payload buffer (MqttClient.cpp:680) — about 1.3 KB of that 21,304-byte pool. Memory is not the constraint. (Note: the research compile reported a 127,728-byte baseline against the project's own measured 127,688 — a 40-byte difference between two `arduino-cli` runs, immaterial but unexplained.)

Stack overflow on this board is silent, not a fault: the Renesas core disables the RA4M1 stack pointer monitor at startup (`R_MPU_SPMON->SP[0].CTL = 0;`, cores/arduino/main.cpp:72–83). So "1.3 KB of 21 KB" is comfortable but unverifiable at runtime.

TLS on 8883 is real, not HTTP-shaped: `WiFiSSLClient::connect(host, port)` passes the port straight through as `AT+SSLCLIENTCONNECTNAME=<sock>,<host>,<port>`, and the bridge handler calls `sslclient->connect(host, port)` on a plain ESP32 `WiFiClientSecure` — a raw TLS stream with no HTTP layer anywhere (`UNOR4USBBridge/cmds_wifi_SSL.h`, `_SSLCLIENTCONNECTNAME`). The bridge has 8 SSL client slots in a pool separate from 8 plain TCP clients, 4 servers and 4 UDP sockets (`at_handler.h`: `MAX_CLIENT_AVAILABLE 8`, `MAX_SERVER_AVAILABLE 4`), so slot count is not the limit. A broker with a public CA needs zero certificate config: the bridge's default trust is the ESP32's on-flash root bundle via `setCACertBundle` from its `cert` partition.

### The four findings that kill a persistent session

1. **No broker exists on GCP.** IoT Core was retired 2023-08-16 with no successor; Google's own guidance is to run your own broker on Compute Engine or GKE (https://docs.cloud.google.com/architecture/connected-devices/mqtt-broker-architecture). Pub/Sub is HTTP/REST and gRPC only (https://docs.cloud.google.com/dataflow/docs/guides/templates/provided/mqtt-to-pubsub). Cloud Run terminates HTTP/1, HTTP/2, gRPC and WebSockets only — no raw TCP — and a WebSocket is a long-running request capped at 60 minutes (https://docs.cloud.google.com/run/docs/triggering/websockets), which would force 24 reconnects a day and needs `min-instances=1` at ~$44.71/month. So MQTT means a third-party broker or a VM you patch: a permanent architectural addition to the delivery path of an unserviceable device.
2. **The existing firmware destroys TLS sockets on purpose.** `WifiClient::ensureWifiConnection()` calls `WiFi.end()` unconditionally whenever `WiFi.status() != WL_CONNECTED` (src/WifiClient/WifiClient.cpp:26), and `CWifi::end()` sends `AT+SOFTRESETWIFI`, whose bridge handler deletes every one of the 8 `sslclients`, every server and every UDP socket. A held-open MQTT session is destroyed by the next clock sync or upload that finds the radio momentarily down, and the R4-side client keeps a stale `_sock` until the next `connected()` call notices. Persistence is not compatible with this code as written.
3. **The settings switch kills it.** `CWifi::beginAP()` writes `AT+WIFIMODE=2` and `CWifi::begin()` writes `AT+WIFIMODE=1`; WiFiS3 never selects mode 3. The bridge firmware *does* implement `WIFI_MODE_APSTA` (`cmds_wifi_station.h:225`) but the R4 library gives no way to reach it — so this is a library limitation, not a hardware one, and it is still unfixable without patching the core. Publish-per-datapoint is therefore impossible, and the SD spool plus replay has to stay under every option. (This confirms the `ap-switch-blocks-all-syncing` note at source level, and adds the detail that the silicon can do it.)
4. **MQTT calls block inside the motor-on path.** `loop()` is the actuator's `extensionCallBack`, called on every iteration of `home()`, `max()` and `moveTo()`'s drive loop with the motor energised (src/LinearActuator/LinearActuator.cpp:211–217; `SolarTracker tracker(loop)` at solar-tracker.ino:6). `MqttClient::connect()` spins up to `_connectionTimeout = 30,000 ms` waiting for CONNACK (MqttClient.cpp:71) and a QoS-1 `endMessage()` spins up to 30 s waiting for PUBACK (MqttClient.cpp:209–258). Any single AT command can stall 10 s on `MODEM_TIMEOUT` (Modem.h:13). `hitBoundary()` stall detection only runs between drive-loop iterations, so an end stop hit during one of those would hold the motor against the stop for up to 30 s. This is exactly the hazard the comment at SolarTracker.cpp:121–134 was written to prevent.

Two more things worth knowing. At-least-once delivery collapses the benefit anyway: PubSubClient's `publish()` has no QoS parameter at all (QoS 0 only), and ArduinoMqttClient's QoS 1 is a blocking call — so "full logs, guaranteed" means QoS-1 publishes confined to the same not-moving window the HTTPS sync already uses, which is batching over a different protocol. And the closest documented analogue to the persistent design already fails in the field: Arduino IoT Cloud on the UNO R4 WiFi *is* a persistent MQTT-over-TLS session through this same bridge, and it is widely reported to drop after 3–12 hours with no automatic recovery, requiring a manual reset (https://forum.arduino.cc/t/iot-cloud-disconnect-after-a-few-hours-uno-r4-wifi/1280222).

### mTLS is impossible on this board

`WiFiSSLClient`'s public API is `setCACert(const char*)` and `setEccSlot(int, const byte[], int)` — there is no `setCertificate()` or `setPrivateKey()`. ArduinoCore-renesas issue #499, "add full support for mTLS to WiFiSSLClient", has been open since 2025-10-10 with no maintainer response and states outright that connecting to AWS IoT Core is impossible with this library (https://github.com/arduino/ArduinoCore-renesas/issues/499). AWS IoT Core's username/password fallback needs a custom authorizer plus port 443 with the ALPN extension set to `mqtt` and SNI (https://docs.aws.amazon.com/iot/latest/developerguide/custom-auth.html) — WiFiS3 exposes no ALPN API, and `setEccSlot` needs an ATECC508/608 the UNO R4 WiFi does not have. **AWS IoT Core is ruled out. Every broker choice is constrained to username/password over server-authenticated TLS.**

### Contradictions with this project's own earlier notes

- **src/WifiClient/WifiClient.cpp:20–25** says the ESP32 "has few enough of them [sockets] that one held listener is the difference between a request that works and one that cannot connect at all." The bridge actually has 8 SSL client slots, 8 plain TCP slots, 4 servers and 4 UDP sockets in separate pools. The measured failure — clock sync succeeds, the upload immediately after it is refused — was therefore almost certainly **ESP32 heap exhaustion**, not slot exhaustion: the module is an ESP32-S3-MINI-1-N8 with 512 KB SRAM built with PSRAM disabled, and each live `WiFiClientSecure` holds an mbedTLS context for the life of the socket. This matters because it changes what "two concurrent TLS sockets" costs, and there is **no AT command that reports bridge free heap** — the most likely failure mode is the one that cannot be instrumented from the sketch. (Medium confidence; the `WiFi.end()` workaround is correct either way, for a different reason than the comment gives.)
- **SolarTracker.cpp:56–59** says the ESP32 "stops handing out listening sockets after four bring-ups in a power session." That number now has a source: `MAX_SERVER_AVAILABLE 4`. The observation was right and the mechanism is a per-bring-up server-slot leak.
- **docs/ota-esp32-bridge.md** states "The current firmware is 126,944 bytes." It is now 127,688 bytes (48% of 262,144), measured with `arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi`. The 122,880-byte `InternalStorage` ceiling remains the closed path; nothing in this PRD approaches the real 262,144 limit.
- **The premise that MQTT "turns the radio on" is wrong.** The firmware already stays associated 24/7: `ensureWifiConnection()` returns immediately at `WL_CONNECTED`, nothing tears the station down after a sync, and `WiFi.disconnect()` only runs after a GET fails all 10 attempts (WifiClient.cpp:91). So the argument against persistence is AT-command polling and mbedTLS context lifetime, not power draw. (Medium confidence on the power figures: ~97 mA idle, 110–175 mA using WiFi, ESP32-S3 TX peaks 180–240 mA.)

### Options

| | A. Keep batched HTTPS (recommended, with a cadence bump) | B. MQTT batched at poll time | C. MQTT persistent session |
|---|---|---|---|
| Shape | 1 TLS socket opened, used, `stop()`ed per POST; `WiFi.end()` on reconnect | connect → QoS-1 publish → disconnect inside the existing gate | session held open, `poll()` pumped from `loop()` |
| New infrastructure | none | EMQX Serverless + webhook rule | same, plus it must stay up 24/7 |
| Worst-case row age | 60 min today; **10 min** with `SYNC_EVERY_HOURS` replaced by a 10-minute timer | 10 min | seconds while associated, 60+ min whenever the switch is on |
| Flash / RAM | 127,688 / 11,212 (unchanged) | +3,424 / +252 (+~1.3 KB heap) | same as B |
| Blocking added to motor-on path | none (gated) | none if gated the same way | up to 30 s per `connect()`, 10 s per AT command |
| Survives `ensureWifiConnection()` | yes, by design | yes | **no** — `AT+SOFTRESETWIFI` deletes the socket |
| Survives the settings switch | yes (spools, replays) | yes (spools, replays) | no |
| Downlink | no | no (would need a subscribe in the gated window) | yes, this is the only real win |
| Server change needed | none | none, if the payload is the raw CSV row | none |
| Cost | $0.00 | $0.00 | $0.00 + a service that must never go down |
| Build + soak | 2 h + 48 h | 3–4 days + 3–5 days | 3–4 days + weeks, and it still drops |

**Recommendation: A.** At 144 rows/day the entire delivery problem is a timer constant. Option B is defensible if a downlink is genuinely wanted, and if built must be built exactly as described below. Option C should be written off permanently — it removes all three of the disciplines that make the current network path reliable (per-request `stop()`, `WiFi.end()` on reconnect, no radio work while the motor runs) and the same architecture already fails every 3–12 hours in Arduino's own product on this board.

### Option A, concretely

Replace the alarm-hour trigger with a wall-clock timer so a sync is queued right after each row is spooled, and let `serviceSync()` keep gating it:

```cpp
// src/SolarTracker/SolarTracker.cpp, in pollSensorData()
if (millis() - telemetryTimer > TELEMETRY_INTERVAL)
{
  spoolTelemetryRow();
  telemetryTimer = millis();
  if (!appMode) syncPending = true;   // serviceSync() still holds the gate
}
```

That makes 144 POSTs/day instead of 24, each carrying one or two rows. Cloud Run: 4,320 requests/month against 2,000,000 free → $0.00. GCS: one object per POST is 4,320 Class A ops/month against 5,000 free → $0.00, but only 14% of headroom left, so **do not** move to per-row objects or per-row publishing without batching. `syncClock()` runs on the same path, so also decide whether to keep setting the RTC 144 times a day or gate the clock sync to once an hour — 144 `setDateTime()` writes a day is more I2C traffic on a bus whose wedge root cause is still open.

### Option B, concretely, if a downlink is wanted

Topology: device → EMQX Cloud Serverless (`mqtts` 8883, username/password) → HTTP Server rule action → the existing Cloud Run `/data`. The GCP Pub/Sub sink is **not** available on Serverless (Dedicated Flex only), but the HTTP Server action is (https://docs.emqx.com/en/cloud/latest/create/serverless.html).

Rule:

```sql
SELECT payload FROM "tracker/+/telemetry"
```

Action: `POST https://test-image-188091506191.us-central1.run.app/data`, header `content-type: text/csv`, body `${payload}`.

The content-type matters more than it looks. `server/routes.js:19` installs `express.text({ type: "text/csv", limit: "1mb" })` and nothing else parses a body for `/data`; `parseRow()` then requires exactly 8 comma-separated fields with a status in `ACTIVE|NIGHT|SAFE|AWAY`. A JSON webhook body yields `raw = ""` and a **200 with `{saved: 0}`** — the silent-drop behaviour already proven by curl. So publish the raw row from `Telemetry::formatRow()` as the payload and forward it verbatim; the server needs no change at all.

Board side, ArduinoMqttClient 0.1.8 is the right library of the three: `+3,384` flash / `+204` RAM isolated, it supports QoS 1, its `poll()` is genuinely non-blocking at QoS 0, and it is the one Arduino regression-tests on this board (it is a dependency of ArduinoIoTCloud, which ships for `renesas_uno`). PubSubClient 2.8 is smaller (`+2,088` / `+80`) but cannot publish above QoS 0, defaults `MQTT_MAX_PACKET_SIZE` to 256, and its `readByte()` spins up to `MQTT_SOCKET_TIMEOUT` (15 s) — reject it. 256dpi MQTT 2.5.3 costs `+5,016` / `+268` and also has a blocking read. All three compile clean on renesas_uno 1.6.0; the 2023 `strnlen` error against PubSubClient (knolleary/pubsubclient#1016) does not reproduce.

```cpp
// src/Telemetry/MqttPublisher.h  — one gated session per window, nothing held open
class MqttPublisher
{
public:
  // Returns true only on a confirmed QoS-1 PUBACK. Caller deletes the chunk on true.
  bool publishChunk(const String &csvChunk);

private:
  WiFiSSLClient tls;          // +1 KB heap FifoBuffer, second SSL slot of 8
  MqttClient mqtt{tls};       // setKeepAliveInterval(300000) — never relied on
};
```

Call it only from inside `serviceSync()`'s window, never from `loop()`:

```cpp
// SolarTracker::serviceSync() — the only safe place for any network call
if (status == SAFE || actuator.status != LinearActuator::STOP) return;
```

The SD spool has to change shape too: today `uploadLogs()` deletes a chunk on HTTP 200, and the MQTT equivalent is an at-least-once outbox keyed on the QoS-1 packet id, so a PUBACK that arrives after a disconnect does not lose or duplicate a chunk. Budget a day for that alone.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| A persistent session drops and never recovers, and the unit goes silent — **makes a site visit more likely** | high (Arduino's own IoT Cloud does this every 3–12 h on this board) | telemetry stops; no remote fix without a watchdog | do not build Option C; land the watchdog + bridge-health reset PRD first |
| An unguarded MQTT call blocks up to 30 s inside the energised drive loop and the panel overruns or pushes into an end stop | medium if gating is missed | mechanical damage, **site visit** | gate every call at `serviceSync()`'s two tests; measure the drive-loop iteration rate first (Requirement 5) |
| Second concurrent TLS socket exhausts the PSRAM-less ESP32's heap; the telemetry POST or OTA download fails | medium, and **unobservable** — no AT command reports bridge free heap | intermittent silent delivery loss | never hold two TLS sockets open at once; one socket per gated window |
| Continuous network activity confounds the still-open DS3231 wedge diagnosis | high if MQTT lands before the remaining bench steps | the motor-current hypothesis becomes untestable forever; permanent unknown | run the supply-present and motor-moving bench steps before any network change (Requirement 11) |
| EMQX Serverless changes its free tier, has an outage, or lands in an AP region while Cloud Run is in us-central1 | low–medium (region coverage is thin; medium confidence) | telemetry path depends on a vendor you do not control | keep HTTPS as the delivery path; treat MQTT as additive, never as the only route |
| `/data` has no authentication, and a webhook makes a second party a publisher into it | low | garbage rows in `raw/`; server drops malformed ones already | add a shared-secret header check when the webhook is created; the endpoint is already public today |
| Bridge firmware below 0.6.0 hits an SSL regression on the new port (issue #50 broke SSL on 443 after 0.4.1) | medium until the bridge is flashed | MQTT never connects, or connects then fails | flash the bridge to 0.6.0 on the same USB visit as the OTA work; **this visit is unavoidable either way** |
| Silent stack overflow from added heap use (SPMON disabled in the core) | low (1.3 KB of 21,304) | memory corruption with no fault | log free RAM each poll; bench-soak 48 h before flashing |

## Acceptance criteria

- [ ] A written one-line answer to "do we want a downlink?" exists in this file or in `SPEC.md`, and the option chosen follows from it.
- [ ] Cloud rows in `gs://solar-tracker-442903-telemetry/raw/` show a maximum gap between a row's `time` field and its containing object's timestamp of ≤ 10 minutes, across a 24-hour window with the settings switch off.
- [ ] Serial log shows a completed upload immediately after each `Spooled row into chunk …` line, and no `Chunk N failed` lines across 24 hours.
- [ ] Serial log shows zero network activity between an actuator move's start and `actuator.status == STOP` — grep the log for a sync line falling inside a move.
- [ ] Bench: the drive-loop iteration rate is measured and recorded in `bench/README.md`, with the resulting worst-case position error for a 10 s and a 30 s stall stated in mm or percent extension.
- [ ] Bench: 48 hours unattended with zero manual power cycles, free RAM flat at the end, and the spool empty.
- [ ] If MQTT was built: `curl`-equivalent proof that the EMQX webhook's exact headers and body produce `{"saved":1}` from `/data`, not `{"saved":0}`.
- [ ] If MQTT was built: a compile report showing flash ≤ 135,000 bytes and globals ≤ 11,600 bytes.
- [ ] If MQTT was built: the EMQX spend limit is set to 0 and a screenshot or console note records it.
- [ ] Neither the SD card nor the RTC was wiped or reset at any point (verified by `00_card_inventory` before and after, and by the RTC still holding time across the change).

## Open questions

- Does Zach actually want a downlink? Everything about MQTT's cost/benefit hinges on this, and nothing in the firmware uses one today.
- The whole memory and bridge-behaviour case rests on a research pass whose adversarial verification step **did not complete**. Specifically unverified and load-bearing: the `+3,424` flash / `+252` RAM figure (one local compile, and a 40-byte baseline discrepancy against the project's own number); the 8-SSL-slot and raw-`WiFiClientSecure` claims (read from bridge source on GitHub, not exercised on this unit's older bridge firmware); the EMQX free-tier quotas and the availability of the HTTP Server action on Serverless (vendor docs only, not confirmed in the console).
- Is EMQX Serverless available in a region near us-central1, or does the broker land on AWS Asia-Pacific while Cloud Run sits in us-central1? Functionally fine over a webhook, but confirm in the console before committing.
- What is the actuator drive-loop iteration rate? Until it is measured, the per-call blocking budget cannot be turned into a position-error bound, and Requirement 5 cannot be signed off.
- If the cadence goes to 10 minutes, should `syncClock()` stay on the same path? 144 RTC writes a day on a bus with an unexplained wedge is a change nobody asked for.
- ESP32 heap is the suspected cause of the socket failure the `WiFi.end()` comment describes, and there is no way to read it. Is it worth asking upstream for an `AT+HEAP` command, or adding one to a locally built bridge image, before trusting two concurrent TLS sockets?
