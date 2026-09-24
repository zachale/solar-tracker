# Over-the-air updates

Status 2026-09-24: **the pipeline works up to the download, and the download does not yet
work on this unit.** Everything on this page up to "Where it stops" is verified on hardware.

## How it works

1. `build.sh <version>` compiles the sketch with the version baked in, then packages it as an
   `.ota`: LZSS-compressed image behind Arduino's 20 byte header (length, CRC32, magic
   `0x23411002`, compression flag). Every `.ota` is decoded again and compared with its `.bin`.
2. `flash-home.sh <version>` puts an image on the board over USB on the home PC. Use it to
   check an image boots before publishing it.
3. `publish.sh <version>` uploads the `.ota` to `gs://solar-tracker-442903-ota/<token>/`, reads
   it back byte for byte, and only then points `manifest.txt` at it.
4. The tracker (`src/OtaUpdater`) fetches the manifest 30 s after boot and every 5 minutes.
   A different version sends the ESP32 to fetch `<version>.ota` into its own flash while the
   RA4M1 carries on; once the whole file is down and its CRC checks out, the ESP32 reprograms
   the RA4M1 and restarts it.

Builds made any other way, the IDE included, report `dev` and never update themselves.

## The bucket

Public read by exact object name (`roles/storage.legacyObjectReader` for `allUsers`), no
listing: a request for the bucket gets 403. The random path segment in `.ota_token` is the
only thing standing between the images and anyone who wants them, and this repo is public,
so the token lives in `.ota_token` and the gitignored `secrets.h`, and the firmware never
prints it. The manifest is served `Cache-Control: no-store` so a change is seen at once.

## Verified

| What | Evidence |
|---|---|
| Arduino's packaging tools | Arduino's reference `UNOR4WIFI_Animation.ota` decoded and re-encoded byte-identical |
| Every build | decodes back to its `.bin`; CRC, length and magic checked |
| Bucket | 200, `Content-Length`, not chunked, no redirect, over HTTP/1.1 and 2; listing refused |
| Server side of the download | the ESP32's exact request (`GET … HTTP/1.1`, `User-Agent: Arduino/2.2.0`, `Connection: close`) answered in 0.36 s with the full 111,583 bytes |
| Version handling | images boot and report their own version; a matching manifest gives `up to date`, a blank one gives `no usable manifest` |
| Non-blocking | the bridge runs the transfer as a task in the ESP32's own loop (`otaDownloadProgress` in `cmds_ota.h`); the RA4M1 only reads a counter |

## Where it stops

`OTAUpdate::startDownload()` against the bucket returns `-26` (`Error::Modem`) after exactly
60 s, the RA4M1's `EXTENDED_MODEM_TIMEOUT`. The ESP32 accepts the command and does not answer.
Reproduced twice. After the second attempt the ESP32 stopped answering AT commands entirely,
`+RESET` included, while USB and sketch uploads kept working. Only a power cycle clears that.

What is known:

- The same host answers the manifest GET, made through the bridge's own TLS client and its
  CA bundle. The OTA path differs in using `Arduino_ESP32_OTA`'s client with the CA passed
  through `setCACert()`.
- The bridge stores that CA in a buffer sized to the exact string length, with no NUL
  (`cmds_ota.h`, `_OTA_SETCAROOT`), then hands it to the TLS layer as a C string.
- The bridge queues its background download task even when `startDownload` has failed and
  cleaned up, and that task dereferences the HTTP client without a null check.
- Arduino's own example host, `downloads.arduino.cc`, chains to GTS Root R4 (ECDSA). The bucket
  chains to GTS Root R1 (RSA-4096) through WR2. Both roots are in `src/OtaUpdater/root_ca.h`.

Next, one variant per clean boot, from `bench/15_ota_probe` (built as `out/probe-A|B|C`):

| Run | Request | Separates |
|---|---|---|
| C | the bucket over plain http | TLS from everything else |
| A | Arduino's reference file | whether this bridge's OTA client works at all |
| B | the bucket over https | the failing case, with no tracker around it |

Each needs the ESP32 freshly powered, and a failing run may wedge it again.

`bench/16_bridge_reset` asks the bridge to restart through its `+RESET` command. It does not
help once the bridge has stopped answering, which is the case it was written for.

## Risks that stay after it works

- A failed handshake between the ESP32 and the RA4M1's bootloader leaves the RA4M1 in its ROM
  bootloader until a power cycle. That lives in the bridge firmware.
- Anything that wedges the ESP32 can only be cleared by a power cycle. An RA4M1 watchdog
  resets the RA4M1, not the ESP32.
- On USB at the home PC none of this can strand the board: the ESP32 serves USB and drives
  the RA4M1's bootloader itself. In the field it means a visit.
