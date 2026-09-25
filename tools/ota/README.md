# Over-the-air updates

The tracker updates itself from Cloud Storage, and undoes the update by itself if the new
firmware is not healthy. No step needs a laptop at the tracker, and a bad release cannot
leave it without a way back, with two exceptions that only hardware can close (see
[What this does not cover](#what-this-does-not-cover)).

## Shipping an update

```
tools/ota/build.sh v12            # build and package; refuses anything unsafe to ship
tools/ota/flash-home.sh v12       # put it on the bench board over USB first
tools/ota/publish.sh v12          # upload, read back, then point the manifest at it
```

The tracker checks the manifest every five minutes. From then on it needs nobody:

1. It reads its rollback copy back from the SD card and compares it, byte for byte, with the
   firmware it is running. No match, no update.
2. It downloads v12 onto the ESP32 in the background while it keeps tracking, then reads that
   back and checks it against the size and CRC-32 in the manifest. Then it checks the SD
   copy once more.
3. It installs v12 **on trial** and restarts.
4. v12 has to prove it can still be updated: fetch the manifest after running for three
   minutes. Then it is confirmed, and written from its own flash to the SD card as the new
   rollback copy.

**To undo**, point the manifest back: `tools/ota/publish.sh v11`. A version still on trial
rolls back at once from the copy on the SD card. A confirmed one takes v11 as a normal
update.

A rollback writes the SD copy onto the ESP32 (about 600 small writes over the AT link), reads
it back from there, and only then flashes it. No network is involved.

A version that fails its trial is never installed again. Ship the fix as a new version.
The same goes for one whose download failed three times: the count is kept in data flash, so
it survives restarts, and it only resets when the manifest names another version.

## What happens when something goes wrong

| If the new firmware... | the tracker... | within |
|---|---|---|
| crashes, or hangs, and keeps doing so | resets itself (watchdog) and flashes the old firmware back on the 4th boot | ~1 min for a crash, ~13 min for a hang |
| runs but cannot reach the network | flashes the old firmware back | 15 min of radio time |
| comes up with its watchdog broken | flashes the old firmware back | at boot |
| is corrupt, cut short, for another board, or missing | never installs it: rejected before anything is flashed | one check |
| you change your mind about | flashes the old firmware back when the manifest points at it | next check |

The trial clock only runs while the tracker is allowed to use the radio. With the settings
switch on or the tracker in SAFE it cannot confirm, and that does not count against the new
firmware.

## How it is built

| Piece | What it does |
|---|---|
| `src/OtaUpdater` | the trial state machine, the SD copy, the read-backs |
| `src/Watchdog` | 5.6 s hardware watchdog, fed by a 1 Hz interrupt only while the main loop has moved in the last 180 s. The part cannot be set longer than 5.6 s, and a single AT command can legally take 10 |
| rollback copy | `OTA.BIN` on the SD card, written from the RA4M1's own flash and read back identical. Its size and CRC-32 are kept in state, to check it while a trial is running |
| the ESP32 | holds one image, `/a.bin`, only on its way to the RA4M1. Everything else OTA code ever put there is deleted before each image goes on |
| state in RA4M1 data flash | two records in separate erase blocks with a generation counter, so a power cut mid-write loses at most the newest change. Reflashing does not touch it |
| manifest | `<version> <bytes> <crc32>` of the image, `Cache-Control: no-store` |
| bucket | `gs://solar-tracker-442903-ota`, readable by exact path, never listable. The path token in `.ota_token` / `secrets.h` is the only thing keeping images private, and this repo is public, so neither is committed and the firmware never prints URLs |

## Verification

The fault matrix (`verify.sh`), run 2026-09-24 on the bench tracker, **all 15 scenarios
passed** in one unattended run of 77 minutes, with no download needing a retry.
Evidence: [`proofs/20260924T231844Z-matrix-q242318.log`](proofs/20260924T231844Z-matrix-q242318.log).

| Scenario | What was done to it | What the tracker did |
|---|---|---|
| base | first image flashed over USB | armed its watchdog, saved a copy of itself on the SD card |
| hop ×5 | manifest pointed at the next image, five times | downloaded in the background, installed on trial, confirmed, saved the new copy |
| noconfirm | an image that never confirms | rolled back from the SD card at the deadline; rejected for good |
| crash | an image that hard-faults in `setup()` | watchdog reset it; rolled back on boot 4 |
| hang | an image that hangs in `setup()` | watchdog reset it three times; rolled back on boot 4 |
| abort | manifest pointed back mid-trial | rolled back from the SD card at once |
| corrupt | one byte flipped in the download | refused three times at the transfer CRC; never flashed |
| magic | an image for another board | refused three times; never flashed |
| readback | sound image, wrong CRC in the manifest | read back, refused three times; never flashed |
| missing | manifest names an image that is not there | found missing before the bridge was asked; never flashed, no restart |
| recover | one more ordinary update after all of that | installed and confirmed normally |

A rollback from the SD card takes about 90 s: 72 s to write the copy to the ESP32 and read
it back, then the flash.

The four runs before it failed, and each failure was a real finding, fixed before the next
run (their logs are in `proofs/` too):

- `q241926` and `q242027`: a second image on the ESP32 came back cut short. That led to the
  SD card redesign (see "Bugs found" below).
- `q242041`: after a download that failed, the next download crashed the bridge; and an
  image that was not there crashed it and reset the RA4M1, round and round. Fixed by the
  clean bridge restart, the existence probe and the attempt count in data flash.
- `q242157`: the board did the right thing, but the test missed the log lines, which fell in
  the USB gap while the bridge restarted. The test now accepts the line logged after.
- `q242301`: a network stall stopped a download at 62%. The board failed that attempt and
  its retry worked, but the test took any failure as fatal. Update hops now report a
  self-retried failure instead of failing on it; anything that never installs still fails.

With production timing (`prove.sh v10 v11`, 2026-09-25), v11 arrived over the air with
nothing else touching the board: the manifest was pointed at v11 at 00:53 UTC, the tracker
found it at its next five-minute check, downloaded it in 20 s while its loop kept running
(14,082 passes), read it back against the manifest, installed it on trial, confirmed it after
332 s, and saved it to the SD card as the new rollback copy at 01:04. Evidence:
[`proofs/20260925T005112Z-v10-to-v11.log`](proofs/20260925T005112Z-v10-to-v11.log).

v12, the build for the 60-hour run (it finishes the clock's setup when a boot finds the I2C
bus wedged), went the same way with `prove.sh v11 v12`: named in the manifest at 02:05 UTC,
downloaded in 20 s (14,704 loop passes), read back at crc 63df720, installed on trial at
02:10:44, confirmed after 331 s, and saved as the rollback copy at 02:16. Evidence:
[`proofs/20260925T020258Z-v11-to-v12.log`](proofs/20260925T020258Z-v11-to-v12.log).

## What this does not cover

Two failures stop the tracker in a way no software on it can undo. Both are in Arduino's
bridge firmware, and both are also how a USB upload works, so they are not new with OTA:

1. **The flasher cannot reach the RA4M1's bootloader.** It has already reset the RA4M1 into
   that bootloader, and on this failure it returns without resetting it back out
   (`BossaUnoR4WiFI.cpp`). Nothing runs until the power is cycled.
2. **Power is lost while the RA4M1 is being written.** The bootloader then finds no valid
   image and waits for a USB upload.

Both end with the tracker sitting still until the power goes off and on. The fix is a
**timer relay or watchdog relay on the 12 V supply** that cuts power for a few seconds once
a day, or whenever the tracker stops toggling a pin. It is a few dollars and one visit, and
it also covers any hang in the ESP32 itself, which the RA4M1's watchdog cannot reset. Until
it is fitted, the chance of either is small: the first has not been seen in any flash on
this board, and the second needs a power cut in a window of about ten seconds per update.

A rollback also needs the SD card. The copy there is read back and compared with the running
firmware, byte for byte, right before every install, so an update never starts without one.
A card that died during the few minutes of a trial would leave a crashing image with nothing
to go back to.

Not tested here, because they need that relay on the bench: power loss mid-download (harmless
by design, the RA4M1 is not touched until the end) and power loss mid-flash (case 2).

## Bugs found on the way

In Arduino's code, each worked around here:

- **WiFiS3 cuts every AT command at 127 bytes, silently** (`Modem.h` `MAX_BUFF_SIZE 128`,
  `vsnprintf`). A longer download command loses its newline, the ESP32 never runs it, and the
  half-sent line corrupts the next command. The URL is kept short, and `OtaUpdater` and
  `build.sh` both refuse anything that would not fit.
- **The bridge's CRC covers the network stream, not the file**, and a failed write is
  ignored, so a full filesystem truncates silently. Every image is read back.
- **The ESP32's SPIFFS loses space for good on every write that fails for lack of it**, and
  the bridge has no command to check or format it. On this unit, repeated tests with two
  images on the ESP32 left a 512 KB partition able to hold about 190 KB: a second image
  was cut short 10 times out of 10 (`bench/18_slot_stress`), each failure cost about 7 KB
  more, and deleting every file did not bring any of it back (`bench/19_spiffs_reclaim`).
  That is why the rollback copy moved to the SD card and the ESP32 holds one image at a
  time. `build.sh` refuses images over 168 KB. Reformatting the partition needs the bridge in
  download mode over USB, so it is worth doing once before the unit goes to site.
- **The bridge deletes an image right after flashing it**, so nothing on the ESP32 can be
  relied on to still be there later.
- **A file write that fails leaves the bridge's AT parser waiting for data**
  (`cmds_esp_generic.h`, no `continue_read()` on the error paths). Only the rollback writes
  files, and it reads the result back before flashing anything.
- **A failed download crashes the ESP32**, and so does the next download after one that
  failed, a few seconds in. Once, a download of an image that was not there also reset the
  RA4M1 with it, and because the attempt count then lived in RAM, the tracker went round
  that loop every 50 s. Now the RA4M1 checks an image exists with its own HTTP client
  before handing it to the bridge, restarts the bridge cleanly after any failed download,
  and counts attempts in data flash before each one starts.
- `firmwareVersionU32()` returns garbage below bridge 0.5.0; this unit is on 0.6.0.

In this project, all found before or during bench testing: a trial image could have been
promoted to good by a failed rollback or a torn state write; a watchdog armed before its
supervisor would have boot-looped forever; `SCB->VTOR` points at a RAM copy of the vector
table, not at the firmware; the flasher pads its last page.

## Testing

- `tools/ota/verify.sh` — the fault matrix, with fast timers and fault-injected images built
  by `build.sh --fast --fault=...`. `publish.sh` refuses those without `--test`.
- `tools/ota/prove.sh <base> <next>...` — normal updates with production timing.
- Both run against the bench tracker plugged into the home PC, and write the serial evidence
  to `tools/ota/proofs/`.
- `tools/ota/capture-supervisor.ps1` keeps the serial capture running across USB drops and
  marks each restart in the log.
