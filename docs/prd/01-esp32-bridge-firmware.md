# 01 — ESP32-S3 bridge firmware

**Status:** RESOLVED 2026-09-23 — no action required
**Owner:** unassigned
**Depends on:** nothing
**Blocks:** nothing. **This PRD previously blocked 02; it no longer does.**
**Estimate:** done
**Cost:** none

## Outcome

The bridge is on **0.4.1**, read from the hardware on 2026-09-23:

```
2026-09-23T11:31:01Z ESP32 bridge firmware: 0.4.1
2026-09-23T11:31:11Z ESP32 bridge firmware U32: 0x636363
```

OTA support arrives in bridge firmware 0.4.1, so **the OTA path in PRD 02 is available on
this unit today and needs no USB visit.** The premise this document was written on — that a
site visit stood between the tracker and over-the-air updates — was wrong.

### The banner meant nothing

`Please upgrade the firmware` is printed by this project's own code, not by the bridge:
`src/WifiClient/WifiClient.cpp:12` and `src/SettingsServer/SettingsServer.cpp:22`, both
comparing against the core's hardcoded `WIFI_FIRMWARE_LATEST_VERSION "0.6.0"`
(`libraries/WiFiS3/src/WiFi.h:20`). It is an advisory string comparison copied from the
WiFiS3 examples and it gates nothing. It has only ever meant "below 0.6.0".

### Two findings for whoever reads this next

`firmwareVersionU32()` returns `0x636363`, which is `"ccc"` in ASCII — string bytes leaking
through rather than a version number. **Compare the textual `firmwareVersion()` instead.**
PRD 02's update trigger depends on this.

Getting the version out required replacing `while (!Serial)` with a flat `delay(5000)`.
`Serial` reads true on this board with no host attached, so the wait fell through and every
line printed in `setup()` was gone before a capture could attach. Three attempts found
nothing before this was understood. Committed in `e1feaad`.

### The bridge cannot update itself

Asked and answered: the ESP32 self-update path in the bridge firmware is broken —
`_updating_renesas` is never cleared, so the WiFi-firmware branch writes to a null `FILE*`.
The ESP32 can reprogram the RA4M1, never itself. `OTAUpdate`'s `file_path` variants stage
the Renesas image; they are not a route to the bridge's own flash. **If the bridge ever
needs 0.6.0, that is a USB procedure and there is no way around it.**

## If a future upgrade to 0.6.0 is ever wanted

Not needed for anything on the current roadmap. Recorded so the research is not lost.

| Risk | Reality |
|---|---|
| Permanent brick | No confirmed case from the official procedure across ~15 traced forum threads. The ESP32-S3 ROM bootloader is mask ROM and cannot be erased |
| **Erases the RA4M1 sketch** | **Yes, and Arduino documents it.** Save and re-flash the tracker firmware afterwards |
| Toolchain soft-brick | Real. serial-discovery 1.5.0 (2026-06-15 to 2026-07-15) actively probes USB descriptors and corrupts the flash stream, giving `Bad data checksum`. Fixed in 1.5.1; 1.5.2 is current. **Update the IDE before starting** ([serial-discovery#128](https://github.com/arduino/serial-discovery/issues/128)) |
| Board "dead" afterwards | Usually still parked in ESP Download mode. **Unplug and replug — RESET is not sufficient** |
| Genuinely unrecoverable | Only via SWD/probe-rs overwriting the bootloader and option-setting memory. The official route never touches either |

Recovery ladder, each step taken from a thread where it worked: unplug/replug, then jumper
plus `espflash write-bin`, then simply retry several times, then flash 0.5.2 and upgrade
from there, then `esptool erase_flash` + `write_flash -z 0`, then downgrade to factory 0.1.0
and re-upgrade, then try a different OS.

---

<details>
<summary>Original document, written before the version was known</summary>

## Problem

Every boot of this unit prints `Please upgrade the firmware` — 29 times in `docs/diagnostics/serial-2026-09-10.log` alone (first at line 153). That line comes from `src/WifiClient/WifiClient.cpp:10-12`, which reads `WiFi.firmwareVersion()` into `fv`, compares it against `WIFI_FIRMWARE_LATEST_VERSION` (`"0.6.0"`, in `renesas_uno/1.6.0/libraries/WiFiS3/src/WiFi.h:20`) and then **throws the value away without printing it**. So after months of field logs nobody knows what bridge firmware is actually on the ESP32-S3 — and that number is the gate on OTA, because OTA support first appears in bridge firmware 0.4.1. Research is explicit that the boot message proves only `fv < 0.6.0`, not that OTA is missing; the unit could already be on 0.4.1 or 0.5.2 and be fully OTA-capable today.

## Goal

The exact bridge firmware version running on this board is known, recorded in the repo, and is at least 0.4.1 — reached, if it is not already, without a site visit.

## Non-goals

- Does not implement OTA. No `.ota` packaging, no GCS bucket, no version manifest, no sketch-side `OTAUpdate` calls (PRD 02).
- Does not report the bridge version to the cloud. The server silently drops any row that is not exactly 8 CSV columns (proven by curl), and stealing a ninth field is somebody else's fight.
- Does not add the `99.99.99` → `NVIC_SystemReset()` workaround. An automatic reboot on a string comparison is exactly the kind of un-debuggable remote behaviour this project avoids; if it is wanted it gets its own PRD.
- Does not upgrade the Renesas core. Stays on `renesas_uno` 1.6.0.
- Does not touch the RTC, the I2C recovery work, the SD card, or the settings switch behaviour.
- Does not attempt a remote bridge update. Research found the bridge's own OTA self-update path appears broken in current `main` (see Design).

## Requirements

1. Print the bridge firmware version once per boot, from `setup()`, before any network use and regardless of settings-switch position, on a line that `grep` can find (e.g. `BRIDGE FW: 0.4.1`).
2. Capture that line in a new dated serial log under `docs/diagnostics/` and record the number in `docs/ota-esp32-bridge.md`, replacing its "Print `WiFi.firmwareVersion()` first" blocker with the answer.
3. Stop here and close the blocker if the printed version is ≥ 0.4.1 — no flash, no trip, hand the number to PRD 02. *(unverified: the 0.4.1 threshold rests on per-tag HTTP probes showing `UNOR4USBBridge/OTA.cpp` returning 404 at tag 0.3.0 and 200 at 0.4.1 and 0.6.0, not on a run against this hardware; the adversarial verification pass of the research did not complete.)*
4. Treat a reading of `99.99.99` as "no answer" and re-read after a full power cycle, not a reset-button press. *(unverified: the 99.99.99 behaviour comes from one forum report, not from this unit.)*
5. Flash the bridge only while the board is attached by USB to a host that can immediately recompile and re-upload the tracker sketch, because any bridge flash erases the RA4M1 sketch first (see Design).
6. Physically disconnect and reconnect the board after a bridge flash, before uploading any sketch.
7. Re-upload the tracker sketch at the current HEAD after any bridge flash, and confirm `Setup complete.` plus a real 200 from the telemetry POST before the unit goes back out.
8. Download and checksum both the target 0.6.0 image and the image for the version currently installed **before** starting, so a rollback needs no network at flash time.
9. Re-run `bench/08_net_layers` and `bench/04_wifi_stress` after a bridge flash and compare against the pre-flash run; do not redeploy on a regression.
10. Leave the SD card untouched and never call `rtc.adjust()` anywhere in this work.

## Design

### Step 1 — read the version (do this before anything else)

The board is on USB at `home` right now. That is the whole window: while it is there, this costs a compile and an upload, and nothing else.

Add the print to `solar-tracker.ino`, in `setup()`, ahead of `buttonPanel.setup()`:

```cpp
  // The bridge version gates OTA and has never once been logged, only compared
  // against and discarded in WifiClient::ensureWifiConnection().
  Serial.print(F("BRIDGE FW: "));
  Serial.println(WiFi.firmwareVersion());   // "99.99.99" means the read failed, not a version
```

`WiFi.firmwareVersion()` is an AT query to the ESP32 and does not need an association, so it works with the settings switch in either position and before any `WiFi.begin()`.

While in that file, note two things about the existing check at `src/WifiClient/WifiClient.cpp:11`:

- `fv < WIFI_FIRMWARE_LATEST_VERSION` is an Arduino `String` comparison, i.e. lexicographic, not semantic. It happens to be correct for every published 0.x version, and would be wrong the day a `0.10.x` appears.
- `"99.99.99" > "0.6.0"` lexicographically, so in the documented broken state the warning goes *quiet*. The absence of `Please upgrade the firmware` is therefore not evidence of a healthy bridge.

Three ways to get the number:

| Option | How | What it costs | Verdict |
| --- | --- | --- | --- |
| **A. Print it from the sketch** | two lines in `setup()`, recompile, upload | one normal sketch upload | **Recommended.** Non-destructive, and the line keeps reporting forever, including from the field. |
| B. `arduino-fwuploader firmware get-version` | `-b arduino:renesas_uno:unor4wifi -a $PORT` | **erases the tracker sketch** — the plugin's `GetFirmwareVersion` calls `uploadCommandsSketch()`, which does `bossac --port=$PORT -U -e -w commands.ino.bin -R` ([main.go L154-167, L221-250](https://github.com/arduino/uno-r4-wifi-fwuploader-plugin/blob/main/main.go)) | No. Same answer, but it wipes the running firmware to get it. |
| C. Infer from the boot message | already in every log | free | Useless. Proves only `fv < 0.6.0`, which is the state we are already in. |

### Step 2 — only if the answer is below 0.4.1

Published bridge versions, from Arduino's own uploader index ([`plugin_firmware_index.json`](https://downloads.arduino.cc/arduino-fwuploader/boards/plugin_firmware_index.json)): `0.1.0, 0.2.0, 0.2.1, 0.3.0, 0.4.1, 0.5.2, 0.6.0`. Target 0.6.0 — it is what core 1.6.0 expects, and it is past 0.5.x, which is where the non-blocking `startDownload`/`downloadProgress` API landed (bridge PR #53, core PR #325). On a version that only has the blocking `download()`, `OTAUpdate::download()` raises the modem timeout to `EXTENDED_MODEM_TIMEOUT` = 60,000 ms and waits — a cliff PRD 02 would rather not stand on.

| Option | Command / procedure | Verdict |
| --- | --- | --- |
| **`arduino-fwuploader`, headless** | `arduino-fwuploader firmware flash -b arduino:renesas_uno:unor4wifi -a /dev/ttyACM0 -m ESP32-S3@0.6.0` ([usage docs](https://github.com/arduino/arduino-fwuploader/blob/main/docs/usage.md)) | **Recommended.** Runs over ssh to `home`, where the board already is. No GUI, no moving the hardware to the Mac. |
| Arduino IDE 2 Firmware Updater | Tools → Firmware Updater → select UNO R4 WiFi → Check Updates → pick 0.6.0 → Install, wait for "Firmware successfully installed", then **disconnect and reconnect** ([Arduino Help Center](https://support.arduino.cc/hc/en-us/articles/9670986058780-Update-the-connectivity-module-firmware-on-UNO-R4-WiFi)) | Same plugin underneath. Only if a GUI is genuinely easier — but it needs the board on the machine running the IDE. |
| esp-idf / `espflash` by hand | Short the `ESP_DOWNLOAD` pad to GND, power via USB, board enumerates as an Espressif USB JTAG/serial debug unit, then `idf.py -p $PORT flash` or `espflash write-bin -b 115200 0x0 S3.bin` ([core bootloader README](https://github.com/arduino/ArduinoCore-renesas/blob/main/bootloaders/UNO_R4/README.md), [bridge README](https://github.com/arduino/uno-r4-wifi-usb-bridge/blob/main/README.md)) | Recovery path only. Keep the procedure to hand; do not make it the plan. |

Full sequence on `home`, board on USB, settings switch **off**:

```sh
arduino-cli board list                                     # confirm the port
arduino-fwuploader firmware list -b arduino:renesas_uno:unor4wifi

# Requirement 8: have both images on disk before touching anything
curl -O https://downloads.arduino.cc/arduino-fwuploader/firmwares/ESP32-S3/0.6.0/ESP32-S3.bin
sha256sum ESP32-S3.bin
# expect e4a0e6f9451188b5714c2ec53a6552d2ad59b59f468cd47b695bd19ba752fd3c, 1966080 bytes
# and the same for the version the board is on now, as the rollback image
#   .../firmwares/ESP32-S3/<current>/ESP32-S3.bin   (flash back with -i <file>)

arduino-fwuploader firmware flash -b arduino:renesas_uno:unor4wifi \
  -a /dev/ttyACM0 -m ESP32-S3@0.6.0
# ends with: "Upload completed! To complete the update process please
#             disconnect and then reconnect the board."

# >>> PHYSICALLY UNPLUG AND REPLUG HERE. Do not skip. <<<

arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi .
arduino-cli upload -p /dev/ttyACM0 --fqbn arduino:renesas_uno:unor4wifi .
```

### The two traps, both absent from `docs/ota-esp32-bridge.md`

**A bridge flash erases the tracker sketch.** Both the flash path and the version-read path go through `reboot()` → `uploadCommandsSketch()`, which touches the port at 1200 bps and then runs `bossac --port=$PORT -U -e -w <commands.ino.bin> -R` ([main.go L221-250](https://github.com/arduino/uno-r4-wifi-fwuploader-plugin/blob/main/main.go)). `-e` is erase. The RA4M1 comes out of a bridge update running Arduino's little command sketch, not the tracker — no tracking, no telemetry, no AP — until the real firmware is uploaded again. `docs/ota-esp32-bridge.md:118-122` says the update needs USB and stops there; it does not say that the board is left blank. That is the difference between "a trip with a laptop" and "a trip with a laptop that has the toolchain, the core, and a bench-tested build of HEAD on it."

**Uploading a sketch before the power cycle destroys the bridge firmware.** The board stays in ESP download mode after a flash, and a sketch upload in that state overwrites the USB-bridge firmware itself ([Arduino Help Center](https://support.arduino.cc/hc/en-us/articles/9670986058780-Update-the-connectivity-module-firmware-on-UNO-R4-WiFi)). The board then stops enumerating as an Arduino and only `espflash` with `ESP_DOWNLOAD` shorted to GND brings it back ([Restore the USB connectivity firmware with espflash](https://support.arduino.cc/hc/en-us/articles/16379769332892-Restore-the-USB-connectivity-firmware-on-UNO-R4-WiFi-with-espflash)). This is the documented way people brick these boards.

### There is no remote path, in either direction

- Below 0.4.1 the `+OTABEGIN` command that a remote update would use is precisely what the firmware lacks — the chicken-and-egg cannot be broken over the air.
- Even on current `main`, the bridge's own self-update (the WiFi-firmware `+OTABEGIN`/`+OTADOWNLOAD` path) looks broken: `Arduino_UNOWIFIR4_OTA` sets `_updating_renesas = true` in its constructor and never clears it, so on that path `_file` is still `nullptr` while `write_byte_to_flash()` takes the `_updating_renesas` branch and does `fwrite(&data, 1, 1, nullptr)` per decoded byte ([OTA.cpp](https://github.com/arduino/uno-r4-wifi-usb-bridge/blob/main/UNOR4USBBridge/OTA.cpp), `_updating_renesas` at OTA.h L72, OTA.cpp L36/L58/L66/L138, never assigned false). Research rates this *medium* confidence. Treat the bridge version as fixed once the board leaves the bench, and get it right while it is on the bench.

### The 99.99.99 state

One reporter's board began returning `99.99.99` from `WiFi.firmwareVersion()` after about a day of running, with failed TLS/MQTT connections alongside it; a single reset-button press did not clear it and a second reboot did ([forum thread](https://forum.arduino.cc/t/arduino-uno-r4-wifi-connection-losses-wifi-fw-version-is-reported-as-99-99-99-instead-of-0-4-1-after-reconnection/1276079)). Two things follow. First, a `99.99.99` reading is not a version and must not be recorded as one. Second — worth noting for PRD 02 — that reporter's board was actually on **0.4.1**, i.e. OTA-capable bridges exist in the wild on boards that still print the upgrade warning.

### If the board is already back on site

Do not make a trip for this alone. Batch it with PRD 03 and PRD 05, which also need the board on USB, and sequence it so the sketch that goes back on at the end of the visit is the bench-tested build those PRDs produce — because the bridge flash will erase whatever is on there. One visit, one final upload, one verification pass.

## Risks

| Risk | Likelihood | Impact | Mitigation |
| --- | --- | --- | --- |
| Bridge flashed somewhere the sketch cannot immediately be rebuilt, leaving the tracker running Arduino's command sketch — **makes a site visit more likely**, and a second one | Medium | High | Requirement 5. Flash only at `home` with `arduino-cli`, the core, and a compiled build of HEAD present and verified first. |
| Sketch uploaded before the disconnect/reconnect, destroying the bridge firmware | Medium | High | Requirement 6, and the unplug step is called out in the command block. Recovery is `espflash` with `ESP_DOWNLOAD` shorted to GND — bench only, never on site. |
| 0.6.0 changes socket, AP or timing behaviour that the current `WiFi.end()` / one-socket-at-a-time discipline depends on | Medium | Medium | Requirement 9: `bench/08_net_layers` (L1–L5 walk) and `bench/04_wifi_stress` (associate → TLS GET → `WiFi.end()`) before and after. Roll back with `-i <old ESP32-S3.bin>`, kept on disk per Requirement 8. |
| Version read comes back `99.99.99` and the real version stays unknown | Low | Medium | Requirement 4: re-read after a cold power cycle, and take two readings. |
| Flash aborts part-way and leaves the bridge non-functional | Low | High | Both images already downloaded and checksummed; `espflash` recovery procedure to hand before starting. Happens on the bench, where a dead board is an afternoon, not a drive. |
| On Linux the board fails to enter download mode on a direct USB port | Medium | Low | Known: the uploader plugin's README states the UNO R4 must be on a **USB hub** on Linux or the reboot into download mode fails. Use a hub on `home`. |
| Version is ≥ 0.4.1 but < 0.5.2, so PRD 02 only has the blocking `download()` and its 60 s modem-timeout cliff | Medium | Medium | Go to 0.6.0 anyway while the board is on the bench, even though OTA would technically work — this is the cheap moment. |
| Nothing is wrong and the whole flash was unnecessary | Medium | Low | That is the *good* outcome, and Requirement 3 is what stops the work there. Cost of finding out: one upload. |

## Acceptance criteria

- [ ] A fresh serial capture in `docs/diagnostics/` contains exactly one `BRIDGE FW: <x.y.z>` line per boot, with a real version, not `99.99.99`.
- [ ] `docs/ota-esp32-bridge.md` blocker 1 is rewritten to state the measured version instead of asking for it.
- [ ] If the version is ≥ 0.4.1: the PRD is closed with no flash, and PRD 02 records the number as its starting assumption.
- [ ] If a flash happened: `arduino-fwuploader` printed `Upload completed!`, and the next boot prints `BRIDGE FW: 0.6.0` with no `Please upgrade the firmware` line anywhere in the capture.
- [ ] The tracker sketch at HEAD is back on the board and `Setup complete.` appears in the log.
- [ ] One real telemetry POST returns 200 in the serial log and the matching row appears in `gs://solar-tracker-442903-telemetry/raw/` for that day.
- [ ] `GET /time/est` succeeds and the clock print matches wall-clock EST — the RTC was never adjusted.
- [ ] Settings switch on brings up the `Solar-Tracker` AP at `192.168.4.1`, and switching it back off returns to field mode, both within one switch cycle.
- [ ] `bench/08_net_layers` reports L1–L5 all passing, and `bench/04_wifi_stress` shows `assocFail=0` / `getFail=0` over a run no worse than the pre-flash baseline.
- [ ] The SD card mounts with its existing spool and config intact (`Config doc:` prints real content, not the empty-config warning).

## Open questions

- **What is the version?** Everything downstream branches on it and it takes one upload to find out. Nothing else in this PRD should be started first.
- Is `arduino-cli` plus the `renesas_uno` 1.6.0 core actually installed on `home`, and can it compile HEAD `0367716` cleanly? If not, that is the real prerequisite, not the flash.
- Is `home` Linux, and is the board on a USB hub or a direct port? The plugin README says direct ports fail to enter download mode on Linux.
- How long is the board staying at `home`? The moment it goes back on site, a sub-0.4.1 reading turns into a trip.
- The 0.4.1 OTA threshold is load-bearing for PRD 02 and is **unverified against hardware** — it rests on per-tag HTTP 404/200 probes of `OTA.cpp` in the bridge repo, and the research's adversarial verification pass did not complete. Worth confirming by actually calling `OTAUpdate::begin()` once the version is known.
- Does 0.6.0 change the SSL client slot count (currently `MAX_CLIENT_AVAILABLE` 8) or AP-mode socket behaviour? Unknown until the bench runs; it is the one thing that could make this update a net loss.
- Should the `99.99.99` detection ship at all, and as what — a logged warning, or the forum's `NVIC_SystemReset()`? Recommend a logged warning here and the reset, if ever, in its own PRD with its own bench evidence.


</details>
