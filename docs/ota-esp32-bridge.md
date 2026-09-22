# OTA through the UNO R4 WiFi's ESP32: a possible update path

Status: idea, researched from source, **not tried on hardware**. Written 2026-09-13.

## Why this matters

The tracker sits somewhere only one person can reach, and every firmware change so
far has needed a laptop plugged into the board. An update the tracker can fetch for
itself would remove that.

The earlier OTA attempt used `ArduinoOTA` / `InternalStorage`, which stages the new
image in the RA4M1's own flash and so caps the sketch at 122,880 bytes. The current
firmware is 126,944 bytes, so that path is closed. See the `ota-flash-ceiling` note.

## The idea

The UNO R4 WiFi has two chips. The RA4M1 runs our sketch. The ESP32-S3 is the WiFi
radio, and it is also the chip that programs the RA4M1 when we flash over USB.

Arduino's firmware for the ESP32 has an OTA feature built in. Our sketch only sends it
AT commands; the ESP32 downloads the image to its own storage and then reprograms the
RA4M1 itself. The RA4M1's flash is never used for staging, so the size ceiling does not
apply.

## Source trail

1. **Sketch side**, in the Renesas core the project already uses
   (`renesas_uno/1.6.0/libraries/OTAUpdate/src/OTAUpdate.cpp`): `OTAUpdate::update()`
   does nothing but `modem.write(... _OTA_UPDATE ...)`, i.e. sends `+OTAUPDATE` to the
   ESP32. The whole API is `begin`, `setCACert`, `download`, `verify`, `update`,
   `reset`. Example: `libraries/OTAUpdate/examples/OTA/OTA.ino`.
2. **ESP32 receives the command**:
   [cmds_ota.h#L222](https://github.com/arduino/uno-r4-wifi-usb-bridge/blob/94d5bb2e8c2cb5492345bfb84d787fde65d1c183/UNOR4USBBridge/cmds_ota.h#L222)
3. **Download goes to the ESP32's SPIFFS, not RA4M1 flash**:
   [OTA.cpp#L58](https://github.com/arduino/uno-r4-wifi-usb-bridge/blob/94d5bb2e8c2cb5492345bfb84d787fde65d1c183/UNOR4USBBridge/OTA.cpp#L58),
   [OTA.cpp#L80](https://github.com/arduino/uno-r4-wifi-usb-bridge/blob/94d5bb2e8c2cb5492345bfb84d787fde65d1c183/UNOR4USBBridge/OTA.cpp#L80)
4. **ESP32 programs the RA4M1**:
   [OTA.cpp#L147](https://github.com/arduino/uno-r4-wifi-usb-bridge/blob/94d5bb2e8c2cb5492345bfb84d787fde65d1c183/UNOR4USBBridge/OTA.cpp#L147)
   `return BOSSA.program(file_path, Serial, GPIO_BOOT, GPIO_RST);` It drives the RA4M1's
   BOOT and RESET lines and writes over serial with Bossa / SAM-BA, the same protocol a
   USB upload uses (`boards.txt`: `unor4wifi.upload.tool=bossac`, `protocol=sam-ba`).

## Our own server works; Arduino Cloud is not required

The download is a plain HTTP GET against whatever URL the sketch passes.
[Arduino_ESP32_OTA.cpp](https://github.com/arduino-libraries/Arduino_ESP32_OTA/blob/main/src/Arduino_ESP32_OTA.cpp)
accepts both `http` and `https` and connects to the URL's own host and port. Arduino's
example happens to point at `downloads.arduino.cc`, but nothing ties it there.

What the server has to do:

| Requirement | Why |
|---|---|
| Answer the GET with status **200** | Any other status is `Error::HttpResponse` |
| Send a **`Content-Length`** header | Missing it is `Error::HttpHeaderError`, so no chunked transfer |
| Serve the file directly, **no redirects** | The HTTP client does not follow them |
| Serve a **`.ota`** file, not a raw `.bin` | See below |

Candidate hosts: a public object in the existing `solar-tracker-442903-telemetry`
bucket (or a dedicated one) via `https://storage.googleapis.com/<bucket>/<object>`, or a
Cloud Run route that streams from Cloud Storage. The server's existing
`/update/download/update-v:version.bin` route will not do as it stands: it reads from
Cloud Run's local disk, which does not persist.

### The `.ota` file format

Built from the compiled `.bin` with Arduino's
[`bin2ota.py`](https://github.com/arduino-libraries/ArduinoIoTCloud/blob/master/extras/tools/bin2ota.py)
(after LZSS-compressing it with `lzss.py` from the same folder):

```
bin2ota.py UNOR4WIFI firmware.lzss firmware.ota
```

It prepends a length, a CRC32, the board magic number `0x23411002` for `UNOR4WIFI`, and
a version field with the compression flag set. The ESP32 rejects a file whose magic
number does not match (`Error::OtaHeaderMagicNumber`) or whose CRC does not
(`Error::OtaHeaderCrc`).

### HTTPS certificate

For `https` the ESP32 needs the root CA of the server it is talking to. Our sketch
passes it with `ota.setCACert(root_ca)` (sent as `+OTASETCAROOT`). Google's endpoints
chain to Google Trust Services roots, so that PEM gets compiled into the sketch.

Plain `http` also works, but then the only integrity check is the CRC32, which protects
against corruption and not against tampering. Use `https`.

## What documentation exists

There is no official guide to doing this against your own server. What exists:

- **The official example sketch**:
  [ArduinoCore-renesas `OTAUpdate/examples/OTA/OTA.ino`](https://github.com/arduino/ArduinoCore-renesas/blob/main/libraries/OTAUpdate/examples/OTA/OTA.ino).
  It downloads `https://downloads.arduino.cc/ota/UNOR4WIFI_Animation.ota`, an `.ota`
  file on an ordinary web server. That is the closest thing to documentation.
- **Arduino Cloud's OTA page**:
  [Over-The-Air (OTA)](https://docs.arduino.cc/arduino-cloud/features/ota-getting-started/)
  lists the UNO R4 WiFi and, under "But how does it work, inside?", says the compiled
  file "is stored in a AWS S3 bucket", the board is given its URL, and it downloads,
  checks length and CRC, then flashes. Cloud OTA is the same URL download, pointed at
  Arduino's S3. That is also why `Arduino_ESP32_OTA` defaults its CA to
  `amazon_root_ca`.
- **Library metadata**: `OTAUpdate/library.properties` is version `0.0.1` with an empty
  `url`. No README.
- **Forum**: [OTA Updates for R4 wifi](https://forum.arduino.cc/t/ota-updates-for-r4-wifi/1447354).
  One user served a `.bin` from a Python server and reported the file "got downloaded
  into the ESP32 but would not transfer into the Renesas chip". Unresolved in the
  thread. Serving a raw `.bin` rather than an LZSS-compressed `.ota` would produce
  exactly that, since the header check fails after the download. That diagnosis is an
  inference, not confirmed.

Separately, `JAndrassy/ArduinoOTA` supports the R4 through `InternalStorage`, which is
the path already ruled out by the flash ceiling.

## Blockers to check before building anything

1. **ESP32 firmware version.** OTA first appears in bridge firmware **0.4.1**
   (2024-02-16); 0.3.0 does not have `OTA.cpp`. Every boot log on this unit prints
   `Please upgrade the firmware`, meaning it is below the 0.6.0 the core expects. Print
   `WiFi.firmwareVersion()` first. Below 0.4.1, update the ESP32 firmware over USB before
   anything else.
2. **Trigger must not depend on the RTC.** Uploads went silent on 2026-09-10 because the
   hourly RTC alarm is their only trigger and the DS3231 dropped off I²C. An update check
   needs to run at boot and on a `millis()` timer.
3. **Settings switch.** While the switch is on the radio is an access point and cannot
   reach the internet, so updates can only happen with it off.
4. **A bad image means a laptop trip.** The ESP32 verifies the file, not whether the new
   sketch actually works. Only put an `.ota` in the bucket after the exact file has been
   flashed and run on the bench.

## Trigger: pull only, no push

The server cannot start an update. The tracker sits behind the site router with no
inbound route, so it has to ask. Arduino Cloud only looks like push because the board
keeps an outbound connection open for the cloud to answer down; that costs RAM and
radio time this board cannot spare.

Chosen design:

1. **Piggyback on telemetry.** The reply to each `POST /data` carries the target
   firmware version. If it differs from the running build, the tracker runs the OTA.
   No extra requests; uploads already run four times a day, so an update lands within
   six hours.
2. **Check at every boot.** This doubles as a manual trigger: put the `.ota` in the
   bucket, and a power cycle by whoever is on site applies it within about a minute.
3. **Also on a `millis()` timer**, never only on the RTC alarm, so a dead clock cannot
   block the update that might fix it.
4. **Settings switch off only**, since the access point has no internet.

## Rough shape if it works out

1. Build firmware, convert to `.ota`, upload to the bucket, and set the target version
   the server returns.
2. The tracker learns the target version from the upload reply or the boot check. If it
   differs from its own build number: `begin`, `setCACert`, `download`, `verify`,
   `update`.
3. On success the ESP32 resets the RA4M1 into the new firmware, which then reports its
   version with the next telemetry upload so the bucket confirms what is running.
