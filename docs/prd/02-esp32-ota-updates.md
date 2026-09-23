# 02 — Over-the-air firmware updates via the ESP32

**Status:** Proposed  
**Owner:** unassigned  
**Depends on:** 07 (the trigger compares a firmware version, which needs a field to carry it). **No longer blocked by 01** — the bridge is on 0.4.1 and already supports OTA, confirmed on hardware 2026-09-23.  
**Blocks:** nothing — but every later firmware change ships more cheaply once this works  
**Estimate:** 17–23 h (2.5–3 working days), assuming the bridge firmware is already ≥ 0.4.1. Add a USB trip if it is not.  
**Cost:** ~$0.0007/month, i.e. rounds to zero. One 103 KB object in GCS Standard is 0.000103 GB × $0.020/GB-month = $0.0000021/month. Egress at $0.12/GB: the realistic ~12 updates/year is 1 download/month × 0.000103 GB × $0.12 = $0.000012/month; an aggressive 30 downloads/month with retries is 0.0031 GB × $0.12 = $0.00037/month. Class B GETs: zero extra requests if the version check piggybacks on a request the tracker already makes; a separate hourly GET would be 720/month × $0.004/10,000 = $0.00029/month.

**Do first:** print `WiFi.firmwareVersion()` from the field board and read the number. Everything below is gated on it, and the boot message the unit already prints does not answer the question.

## Problem

Every firmware change so far has needed a laptop plugged into the tracker, and the last debugging round cost ~37 h on the bench plus ~30 h in the field to land one recovery routine. The earlier `ArduinoOTA` / `InternalStorage` path is closed for good: it stages the new image in the RA4M1's own flash and caps the sketch at 122,880 bytes, and today's build is 127,688 bytes (`arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi solar-tracker.ino`). The scar is still in the code — `src/SolarTracker/SolarTracker.cpp:267` reads `// REMOVED OTA UPDATER FOR NOW` in the middle of `dailySync()`. The consequence is that the one repair nobody can do remotely is also the repair every bug needs.

## Goal

A `.ota` build placed in a Cloud Storage bucket is running on the tracker within 30 minutes, with nobody at the site.

## Non-goals

- Not changing the 8-column CSV contract or the server's row parser. The running build number is reported out-of-band (see Design), not as a ninth column.
- Not updating the ESP32 bridge firmware over the air. That needs USB, and the bridge's own OTA self-update path looks broken in current `main` anyway.
- Not adding a watchdog, an A/B slot, or automatic rollback. The RA4M1 has one application region; there is nowhere to keep a second image.
- Not push. No inbound route to the site, no persistent socket, no MQTT.
- Not touching the SD spool format, the `tlm*.csv` files, or the RTC.
- Not fixing `ClockModule::isHealthy()`, the per-loop I2C probe, or the wedge counts smuggled through `wind_peak`. Separate work.
- No Arduino Cloud account, no Arduino IoT Cloud library.

## Requirements

The adversarial verification pass on `/tmp/research.json` did not complete. Every claim below was researched against library source and live `curl`, but none was re-checked by a second pass. Requirements whose **design would change if the claim is wrong** are marked.

1. Log `WiFi.firmwareVersion()` once at boot on its own line, so the bridge version is visible in every captured log rather than inferred.
2. Refuse to attempt an OTA when the bridge version is below `0.4.1`, and say so in the log — that build has no `OTA.cpp` at all and the AT command returns nothing useful.
3. Serve the `.ota` from `https://storage.googleapis.com/<bucket>/<object>` and never from the existing Cloud Run service (unverified: rests on one live `curl -sI --http1.1` showing Cloud Run replies `Transfer-Encoding: chunked` with no `Content-Length`, which is `Error::HttpHeaderError` = −7).
4. Grant the `.ota` object `allUsers:objectViewer` and never construct a signed URL (unverified: rests on reading `Arduino_ESP32_OTA.cpp` L146, which passes only `parsed_url.path()` — a single source read, no hardware test).
5. Put the firmware object in a **dedicated bucket**, not `solar-tracker-442903-telemetry`, so nothing public is ever adjacent to telemetry.
6. Build the `.ota` with `lzss.py --encode` then `bin2ota.py UNOR4WIFI`, producing the 20-byte header with magic `0x23411002`.
7. Compile the stock `root_ca.h` shipped with the core's OTA example into the sketch and pass it to `ota.setCACert()`; do not hand-roll a PEM.
8. Drive the update check from the existing `dailySync()` path only — boot plus the `millis()` timer — and never from the DS3231 alarm.
9. Skip the check entirely while `appMode` is true, since the radio is an access point and has no route to the internet.
10. Perform no OTA work while `actuator.status != LinearActuator::STOP` or `status == SAFE`. The existing `serviceSync()` guard already provides this; the OTA must live inside it, not beside it.
11. Use `startDownload()` / `downloadProgress()` when the bridge is ≥ 0.5.0 so no single AT command can outlast `MODEM_TIMEOUT`, and fall back to the blocking `download()` only on 0.4.x.
12. Abort on any non-zero error from `begin`, `setCACert`, download, or `verify`, leaving the running sketch untouched, and log the failing stage and the numeric code.
13. Apply an update whenever the target build differs from the running build in **either direction**, so a rollback to a known-good build is the same mechanism as an upgrade.
14. Stop attempting after three consecutive failures in one power session, and resume only when the target build number changes or the board reboots.
15. Upload the telemetry spool before applying an update, so the last rows before a reset are already in the cloud if the new image misbehaves.
16. Report the running build number to the cloud on every sync without adding a CSV column, so "what is actually running" is answerable from Google Cloud rather than from serial.
17. Flash the byte-identical `.ota` file onto a bench board and watch it run before that file is uploaded to the bucket; verify byte-identity with `md5`.
18. Write nothing to the SD card and issue no RTC write on any OTA path.

## Design

### What the ESP32 actually does (confirmed in source)

`OTAUpdate::update()` is nothing but `modem.write(... _OTA_UPDATE ...)` — an AT command (`renesas_uno/1.6.0/libraries/OTAUpdate/src/OTAUpdate.cpp:141`). On the other side, the bridge firmware stages the image in **its own SPIFFS**, not in RA4M1 flash: `Arduino_UNOWIFIR4_OTA::initStorage()` calls `SPIFFS.begin()` then `fopen("/spiffs" + file_path, "wb")`, the overridden `write_byte_to_flash()` writes each decoded byte to that `FILE*`, and `update(file_path)` is literally `return BOSSA.program(file_path, Serial, GPIO_BOOT, GPIO_RST);`
→ https://github.com/arduino/uno-r4-wifi-usb-bridge/blob/main/UNOR4USBBridge/OTA.cpp (L64–71, L73–87, L145–148)

So the 122,880-byte `InternalStorage` ceiling genuinely does not apply.

| Number | Value | Source |
|---|---|---|
| ESP32 SPIFFS partition | `0x080000` = 524,288 B, ships blank | partition table in the release asset `UNOR4USBBridge.ino.partitions.bin`; `combine.py` on `main` (offset differs between the two: `0x330000` vs `0x370000`, size identical) |
| What lands in SPIFFS | the **decompressed** image, ~127,688 B today | LZSS `putc` callback writes to the `FILE*` — `Arduino_ESP32_OTA.cpp` L115–118, L233 |
| Headroom | ~4× on the staged image, ~5× on the `.ota` itself | 524,288 / 127,688 |
| Real hard ceiling | **245,760 B** of RA4M1 application flash | `FLASH_IMAGE_START=0x4000`, `FLASH_LENGTH=0x40000` in `variants/UNOWIFIR4/memory_regions.ld` |
| Expected `.ota` size | 95–110 KB, call it ~103 KB | LZSS here is an 11-bit window / 4-bit match (`lzss.c` L6–7) and achieved 0.807 on Arduino's own linked RA4M1 image (36,380 → 29,361 B) |
| Cost of this feature | ~7 KB flash: 6,057 B CA PEM + `OTAUpdate` glue | `root_ca.h` is 6,771 B on disk, 6,056 B of string after unescaping, 5 certificates |

**Correction to our own numbers:** the sketch is at 52% of usable flash, not 48%. `boards.txt` L52 advertises `upload.maximum_size=262144`, but the linker script starts the image at `0x4000`, so 245,760 is what exists. With the CA PEM and glue the build lands near 134,700 B ≈ 55%. Comfortable either way, but the honest denominator is 245,760.

### Where this contradicts `docs/ota-esp32-bridge.md`

This is the part worth reading twice. Two of the three are load-bearing.

| The doc says | Reality | Why it matters |
|---|---|---|
| Candidate host: "a Cloud Run route that streams from Cloud Storage" | **Cannot work.** Cloud Run sends `Transfer-Encoding: chunked` whenever the app sets no explicit `Content-Length`. `ArduinoHttpClient` then leaves `iContentLength` at `kNoContentLengthHeader` (−1), and `Arduino_ESP32_OTA.cpp` L163–176 turns that into `Error::HttpHeaderError` (−7). | Kills the obvious design of reusing the service we already trust. Serve straight from `storage.googleapis.com`. |
| Not mentioned at all | **GCS signed URLs are impossible.** The OTA client issues `_http_client->get(_context->parsed_url.path())` — path only. `ParsedUrl` keeps `_query` separately and `query()` is never referenced anywhere in the library. A signed URL carries its whole authentication in the query string, so the request goes out unsigned and GCS answers 403 → `Error::HttpResponse` (−14). | The firmware image must be **world-readable**. URL expiry cannot be used as access control. |
| "Google's endpoints chain to Google Trust Services roots, so that PEM gets compiled into the sketch" — implying we source a PEM | No new PEM needed. The live chain is leaf(`CN=storage.googleapis.com`) → WR2 → **GTS Root R1**, and the stock `root_ca.h` already holds GTS Root R1, R2, R3, R4 and GlobalSign ECC Root CA R4. | One less thing to get wrong. Copy the file from the core's OTA example verbatim. |
| "Every boot log prints `Please upgrade the firmware`, meaning it is below the 0.6.0 the core expects" | True but weaker than it reads. That warning is `fv < WIFI_FIRMWARE_LATEST_VERSION` where the constant is `"0.6.0"` (`WiFiS3/src/WiFi.h:20`), printed by our own `WifiClient.cpp:10–12`. The unit could already be on 0.4.1 or 0.5.x and be fully OTA-capable. | The difference between "build it this week" and "drive there first". |
| Trigger: "the reply to each `POST /data` carries the target firmware version" | The POST reply body is thrown away — `WifiClient::post()` returns only an `int` (`src/WifiClient/WifiClient.cpp:108–126`) — and the POST only happens when the spool has chunks, so at a clean boot there is no reply at all. | Use the `GET /time/est` reply instead, which already runs on every sync and is already parsed as JSON. |

Also worth knowing before anyone plans an escape hatch: the bridge's own OTA self-update (`WiFiFirmwareOTA`) appears broken on current `main`. `Arduino_UNOWIFIR4_OTA` sets `_updating_renesas = true` in its constructor and never clears it, so on the WiFi-firmware path `_file` is still `nullptr` while `write_byte_to_flash()` does `fwrite(&data, 1, 1, nullptr)` per byte. Treat the bridge version as **fixed once deployed**, and get it right during a physical visit. (Medium confidence — source reading only, no hardware test.)

### Hosting: three options

| Option | Works? | Notes |
|---|---|---|
| Public object at `storage.googleapis.com` | **Yes** | Verified live on HTTP/1.1: `200 OK`, real `Content-Length`, no `Transfer-Encoding`, `x-goog-stored-content-encoding: identity`, `num_redirects=0`. A missing or non-public object returns a clean 403 *with* `Content-Length`, which fails at the status check before any flash is touched. |
| Signed URL to the same object | No | Query string dropped → unsigned request → 403 → −14. |
| Route on the existing Cloud Run service | No | Chunked, no `Content-Length` → −7. Would need an explicit `Content-Length` on a buffered response, which means holding 103 KB in the handler for no benefit. |

**Recommendation:** a dedicated bucket, `gs://solar-tracker-442903-firmware`, one object per build, `allUsers:objectViewer` on the object. The image is LZSS-compressed but not encrypted, so treat it as public: `secrets.h` holds only the Cloud Run URL (`secrets.h:6`) — WiFi credentials live on the SD card in `wifi.jsn` and are never compiled in. Check `constraints/storage.publicAccessPrevention` on project `solar-tracker-442903` before building anything; if the org policy enforces it, the object cannot be made public and this whole design stops here.

### Build pipeline

Both tools live in `arduino-libraries/ArduinoIoTCloud/extras/tools`, **not** in `Arduino_ESP32_OTA` (that path 404s). `lzss.py` is only a ctypes shim that dlopens a shared library built from the `lzss.c` beside it. Put this at `tools/ota/build-ota.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
BUILD="$1"                       # integer build number, e.g. 43
BUCKET=solar-tracker-442903-firmware
OUT=.ota-build

arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi \
  --output-dir "$OUT" solar-tracker.ino

# one-time: cc -O2 -shared -fPIC -o lzss.dylib lzss.c   (in extras/tools)
"$TOOLS/lzss.py" --encode "$OUT/solar-tracker.ino.bin" "$OUT/fw.lzss"
"$TOOLS/bin2ota.py" UNOR4WIFI "$OUT/fw.lzss" "$OUT/solar-tracker-$BUILD.ota"

md5 "$OUT/solar-tracker-$BUILD.ota"     # record this; it is what must be bench-flashed
gcloud storage cp "$OUT/solar-tracker-$BUILD.ota" \
  "gs://$BUCKET/solar-tracker-$BUILD.ota"
gcloud storage objects update "gs://$BUCKET/solar-tracker-$BUILD.ota" \
  --add-acl-grant=entity=AllUsers,role=READER
```

`bin2ota.py` needs the `crccheck` pip package. The header is 20 bytes: `uint32 len` (LE) = 12 + compressed size, `uint32 crc32` (LE) over magic+version+payload, `uint32 magic` = `0x23411002`, then an 8-byte version field with the compression flag set. This was parsed byte-exactly out of Arduino's own shipped `UNOR4WIFI_Animation.ota` (len=29373 matched, magic matched, recomputed CRC32 = `0x250F2964` matched), and `lzss.c` built locally re-encoded their payload byte-identically, so the pipeline is known to reproduce theirs.

### Version handshake: three options

| Option | Extra requests | Problem |
|---|---|---|
| Field on the `POST /data` reply | none | The reply body is discarded by `WifiClient::post()`, and no POST happens when the spool is empty — including at a clean boot. Would need `post()` to return a body. |
| **Field on the `GET /time/est` reply** | **none** | Already runs on every sync (`SolarTracker.cpp:320`) including the boot sync, and the body is already parsed with ArduinoJson. |
| Separate `manifest.json` object in GCS | +1 GET per sync | One more host, one more failure mode, and an unnecessary dependency on the bridge's own CA bundle covering `storage.googleapis.com`. |

**Recommendation:** put it on `/time/est`. Two small changes, both cheap:

*Server* (`server/routes.js` on `feature/telemetry-api` — the branch that is actually deployed):

```js
app.get("/time/est", (req, res) => {
  // ... existing estDate ...
  res.send({
    datetime: estDate.toISOString(),
    firmware: Number(process.env.FIRMWARE_BUILD || 0),
  });
});
```

Publishing an update is then: upload the `.ota`, then set `FIRMWARE_BUILD` on the Cloud Run revision. Rolling back is setting it to the older number — requirement 13 makes the tracker take it either way.

*Sketch*: a new `src/Firmware/Firmware.h` holding one constant, `static const uint16_t FIRMWARE_BUILD = 42;`, bumped by hand with every flash. The URL is built on-device from a fixed base plus that number, so the server can only ever choose a build, never a host:

```cpp
String url = "https://storage.googleapis.com/solar-tracker-442903-firmware/solar-tracker-"
             + String(target) + ".ota";
```

`syncClock()` reads the extra field out of the document it already builds — bump `StaticJsonDocument<200>` to `<256>` for headroom (a stack local, against 21,556 B of free locals+stack):

```cpp
uint16_t target = doc["firmware"] | 0;
if (target != 0 && target != FIRMWARE_BUILD) pendingBuild = target;
```

**Reporting what is running** without a ninth CSV column: append the build to the sync GET as a query parameter — `GET /time/est?fw=42`. Express ignores the unknown parameter, the reply is unchanged, and Cloud Run's request log records the full URL in `httpRequest.requestUrl`. One `gcloud logging read` then answers "which build is on the tracker right now", and the 8-column contract is untouched. Note the asymmetry that makes this work: only the *OTA* client drops query strings; `R4HttpClient` over `WiFiSSLClient` handles them normally.

### Where the check runs

Inside `dailySync()`, replacing the `// REMOVED OTA UPDATER FOR NOW` comment at `src/SolarTracker/SolarTracker.cpp:267`, in this order:

```cpp
void SolarTracker::dailySync()
{
  actuator.checkWeeklyRecalibration();
  syncClock();        // learns target build, reports running build
  uploadLogs();       // get the last rows into the cloud before any reset
  applyPendingUpdate();
}
```

That placement inherits three guards for free, which is the main reason to put it there rather than anywhere else:

- `serviceSync()` (`SolarTracker.cpp:134–146`) already refuses to run when `syncPending` is false, when `appMode` is true, when `status == SAFE`, or when `actuator.status != LinearActuator::STOP`. This is the guard that matters most: `moveTo()` calls back into `loop()` on every iteration of its drive loop **with the motor energised**, and a 60-second download from inside that would drive the panel past its target.
- The `millis()`-driven `monitorSyncTimer` (`MONITOR_SYNC_MS = 1800000`) sets `syncPending` every 30 minutes regardless of the DS3231, so a wedged I2C bus cannot block the update that might fix it. No new timer needed.
- `setup()` calls `dailySync()` at `SolarTracker.cpp:19`, which is **before** `initializeStatus()` at L20 and therefore before the ~90-second home-and-max sweep. A power cycle by whoever is on site applies a waiting update within about a minute.

Residual hole to accept knowingly: `clockModule.setup()` runs at `SolarTracker.cpp:17`, ahead of the boot OTA check, and a failed `Wire.endTransmission()` on this core costs roughly a second per address (ArduinoCore-renesas issue #492 measured a 126-address scan at ~126,000 ms). A wedge at boot delays the OTA check by seconds, not forever — but a *hang* there would take the OTA with it, and there is no watchdog anywhere in this firmware.

### The apply sequence

```cpp
void SolarTracker::applyPendingUpdate()
{
  if (pendingBuild == 0 || otaFailures >= 3) return;

  String url = otaUrlFor(pendingBuild);
  int rc = ota.begin("/update.bin");              // truncates any stale staged image
  if (rc == OTAUpdate::OTA_ERROR_NONE) rc = ota.setCACert(root_ca);
  if (rc == OTAUpdate::OTA_ERROR_NONE) rc = otaDownload(url);   // see below
  if (rc == OTAUpdate::OTA_ERROR_NONE) rc = ota.verify();
  if (rc != OTAUpdate::OTA_ERROR_NONE) {
    otaFailures++;
    Serial.println("OTA error code=" + String(rc) + " build=" + String(pendingBuild) +
                   " failures=" + String(otaFailures));
    return;                                       // old sketch keeps running
  }

  Serial.println("OTA flashing build " + String(pendingBuild) + " now, this board will reset");
  Serial.flush();
  ota.update("/update.bin");                      // does not return
}
```

`update()` never returns: `BossaUnoR4WiFi::program` calls `setProgramMode`, which resets the RA4M1 at the *start* of the operation. The official example's error check after `ota.update()` is unreachable code — do not copy it, and log before the call, not after.

`otaDownload()` prefers the non-blocking API. `download()` raises the modem timeout to `EXTENDED_MODEM_TIMEOUT` = 60,000 ms and waits (`Modem.h:12–13`, `OTAUpdate.cpp:60,69`); the ESP32 must fetch ~103 KB, LZSS-decode it and write it to SPIFFS **one byte at a time** inside that window or the RA4M1 gives up while the ESP32 carries on. `startDownload()` + `downloadProgress()` (bridge ≥ 0.5.0) removes that cliff:

```cpp
int otaDownload(const String &url) {
  int rc = ota.startDownload(url.c_str(), "/update.bin");
  if (rc < 0) return rc;
  unsigned long began = millis();
  for (;;) {
    int p = ota.downloadProgress();
    if (p < 0) return p;                          // negative is an error code
    if (p >= expectedBytes) return OTAUpdate::OTA_ERROR_NONE;
    if (millis() - began > 180000) return -1;     // hard cap, give up and keep running
    delay(250);
  }
}
```

Error codes worth memorising when reading a log, all from `Arduino_ESP32_OTA.h` L35–51:

| Code | Meaning | Where it fails |
|---|---|---|
| −7 | `HttpHeaderError` | no `Content-Length` — i.e. someone pointed it at Cloud Run |
| −10 | `OtaHeaderCrc` | caught by `verify()` **before** `update()` runs, so BOSSA never starts |
| −11 | `OtaHeaderMagicNumber` | wrong board magic; aborts mid-download |
| −14 | `HttpResponse` | status was not exactly 200 — 403 (not public), 404, or any redirect |
| −25 / −26 | `Library` / `Modem` | bad arguments, or the AT command itself failed |

None of these touches RA4M1 flash. The board keeps running the old sketch.

## Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| A flash completes and the image is structurally valid but the sketch does not work (bad build, hang, crash before the OTA check) — **makes a site visit certain, not just more likely** | Medium | High | Bench-flash the byte-identical `.ota` and watch it run a full sync before upload (req. 17). Keep the OTA check ahead of the homing sweep so a fresh boot always gets a chance to pull a fix. Nothing else helps: the ESP32 is a passive AT slave that only acts when the RA4M1 asks, and bootloader re-entry needs hands on the board — a double-tap of RESET (Arduino documents a 120-second window) or a 1200 bps USB touch from an attached computer. `BossaUnoR4WiFi::program` also discards `flash()`'s return value and returns 1 regardless, so the sketch is never told programming failed. |
| Bridge firmware is below 0.4.1 | Unknown until measured | High | Read `WiFi.firmwareVersion()` first. Below 0.4.1 there is no remote path at all: short `ESP_DOWNLOAD` to GND, power over USB, flash 0.6.0 with `espflash`/`arduino-cli`. Do it once, properly, and treat the version as fixed thereafter. |
| Org policy `storage.publicAccessPrevention` blocks the public object | Medium | Medium | Silent 403 → −14, old sketch keeps running, no damage. Check the policy before writing code; if it is enforced, this PRD does not ship. |
| Firmware image is world-readable | Certain by design | Low | Nothing secret is compiled in: only the Cloud Run URL (`secrets.h:6`). WiFi credentials live on the SD card. Keep it in a separate bucket from telemetry. |
| Power cut mid-BOSSA leaves flash erased | Low | High | The bootloader occupies `0x000000–0x003C28` and the sketch links at `0x4000`, so BOSSA cannot erase it; with flash at `0xFF` the `app_valid` test `((*(uint32_t*)SKETCH_FLASH_OFFSET) & 0xFF000000) == 0x20000000` fails and the bootloader simply waits. Not bricked — but it waits for USB, so it is still a site visit. Prefer applying updates at a time of day when site power is stable. |
| The ~2-minute flash + reset + re-home window cannot respond to wind | Low | Medium | The sync is already refused while `status == SAFE`, so an update never starts during a wind event; an event arriving mid-flash is uncovered, and that is accepted. |
| Someone leaves the settings switch on, so nothing reaches the internet (`+WIFIMODE=2`, never AP+STA) | Medium | Low | Log "OTA skipped: app mode" every time, so the captured log says why nothing happened. |
| Endless retry loop burning radio time every 30 minutes | Medium | Low | Cap at three consecutive failures per power session; resume only on a changed target build or a reboot (req. 14). |
| Blocking compounds — a wedged RTC plus a stalled AT command with no watchdog | Medium | Medium | The 180-second hard cap on `downloadProgress()` polling, and the fact that no single AT command exceeds `MODEM_TIMEOUT` on the non-blocking path. |

## Acceptance criteria

- [ ] Boot log contains a line giving the bridge firmware version, and it reads ≥ 0.4.1.
- [ ] `curl -sI --http1.1 https://storage.googleapis.com/solar-tracker-442903-firmware/solar-tracker-<n>.ota` returns `HTTP/1.1 200`, a `Content-Length`, and no `Transfer-Encoding`.
- [ ] `md5` of the object in the bucket equals the `md5` of the file that was flashed and run on the bench board.
- [ ] A bench board running build N, pointed at a service with `FIRMWARE_BUILD=N+1`, logs `OTA flashing build N+1 now`, resets, and comes back logging build N+1 — inside one sync cycle.
- [ ] Cloud Run request logs show `GET /time/est?fw=N+1` after that update, and no `?fw=N` afterwards.
- [ ] Deliberate failure injection, each leaving the board still running the old build and logging the exact code: wrong magic (−11), corrupted CRC (−10), non-public object (−14), the Cloud Run URL (−7), and a URL with a query string (−14).
- [ ] A rollback works: setting `FIRMWARE_BUILD` back to N brings build N back.
- [ ] With the settings switch on, the log says the OTA was skipped and no download is attempted.
- [ ] Three consecutive failures stop further attempts in that session, and the log says so.
- [ ] Before and after an update, `tlm*.csv`, `config` and `wifi.jsn` are unchanged, and the RTC's `unix=` reading is continuous across the reset.
- [ ] Sketch size after the change is under 150,000 bytes of the 245,760 usable.
- [ ] A power cut forced during the BOSSA window leaves the board in the bootloader and recoverable over USB — run this once on the bench so the failure is a known shape, not a surprise.

## Open questions

- **What is `WiFi.firmwareVersion()` on the field unit?** Gates everything: ≥ 0.4.1 means OTA works today, ≥ 0.5.0 additionally gives the non-blocking download, below 0.4.1 means a USB trip before any of this. The board is currently on the home PC (ssh host `home`), so this is one flash and one log line away.
- **Does the org policy on `solar-tracker-442903` allow public objects?** If `constraints/storage.publicAccessPrevention` is enforced, there is no working hosting option and the design has to change.
- **Both load-bearing contradictions are single-source.** The query-string drop is one read of `Arduino_ESP32_OTA.cpp` L146 plus a `grep` finding no use of `query()`; the Cloud Run chunking is one `curl -sI`. Confirm both on the bench by deliberately pointing a board at a signed URL and at a Cloud Run route and checking for exactly −14 and −7. Cheap, and it either retires the risk or rewrites the design.
- **What is the real LZSS ratio on our linked image?** The ~103 KB estimate extrapolates from 0.807 measured on Arduino's animation sketch (medium confidence). Run the encoder once on our own `.bin` and record the actual number; the conclusion does not change unless it lands above ~500 KB, which it cannot.
- **Where does the target build number live server-side** — a Cloud Run env var, or an object in the firmware bucket the server reads? The env var is simpler and makes publishing two commands; it also means a revision deploy on every publish.
- **Should the build number be generated or hand-bumped?** A hand-bumped constant in `src/Firmware/Firmware.h` is the simplest thing that works, and it is also the thing most likely to be forgotten in the middle of a debugging session — which would mean the tracker declines the fix it needs.
- **Which machine owns the pipeline?** Flashing and log capture happen on the home PC's checkout, but `bench/runone.sh` hard-codes a Mac serial port (`/dev/cu.usbmodemC04E3011DB182`). Decide where `tools/ota/build-ota.sh` and the bench validation sketch (`bench/14_ota_failures`, following the existing numbering) are meant to run before writing them.
