# Over-the-air updates

Status 2026-09-24: the download failed on this unit until the cause below was found and
fixed. The end-to-end run on the fixed build is recorded at the bottom of this page.

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

## The bug that stopped it, found 2026-09-24

**WiFiS3 cuts every AT command at 127 characters, silently.** `Modem::write()` formats the
command with `vsnprintf` into `tx_buff[MAX_BUFF_SIZE]`, and `MAX_BUFF_SIZE` is 128
(`libraries/WiFiS3/src/Modem.h:14`, `Modem.cpp:87`). The download request is one command,

```
AT+OTADOWNLOADSTART=<url>,/update.bin\r\n
```

and with the first bucket path it came to 129 bytes. The closing newline never left the
RA4M1. The bridge ends a command only on `\n` (`parser.cpp`, a lone `\r` is ignored), so it
sat waiting for the rest of a line that was never coming, the RA4M1 waited out its 60 s
`EXTENDED_MODEM_TIMEOUT`, and startDownload returned `-26`. The unfinished line then swallowed
the next command sent to the bridge, which is why the bridge looked wedged afterwards.

Found by probing one variable at a time with `bench/15_ota_probe`, each run on a freshly
restarted bridge:

| Probe | Request | Command | Result |
|---|---|---|---|
| A | Arduino's reference file, downloads.arduino.cc | 90 bytes | works, 29 KB in 5.8 s |
| D | Arduino's file copied to the bucket, short path | 95 bytes | works, 29 KB in 2.2 s |
| C | our image, token path, **plain http** | 128 bytes | `-26` after 60 s |
| E | Arduino's file at our token path | 128 bytes | `-26` after 60 s |
| B | our image, token path, https | 129 bytes | `-26` after 60 s |

C ruled out TLS, D ruled out Cloud Storage, E ruled out the image. The one thing every
failure shares is a command of 128 bytes or more. GCS itself answered the ESP32's exact
request, replayed from a laptop, in 0.3 s.

The fix is to stay under the limit and to refuse, loudly, anything that would not:

- the bucket path token went from 32 hex characters to 22 URL-safe ones, still 128 bits, which
  brings the command for a two-character version to 119 bytes
- `OtaUpdater` measures the command against the library's own `MAX_BUFF_SIZE` before sending
  it, and fails with a message instead of hanging
- `build.sh` refuses a version name that would push the command past 127 bytes

Worth reporting upstream: the truncation is silent, and any sketch passing a long URL or
hostname through WiFiS3 can hit it.

### Tools from the investigation

- `bench/15_ota_probe` runs one download variant per boot, never flashes, never prints the URL,
  and replays its result every 10 s without touching the modem again.
- `bench/16_bridge_reset` restarts the ESP32 through its own `+RESET` (`ESP.restart()`, an
  ordinary boot). It clears a bridge left holding half a command. Wait for the board to drop
  off USB and come back before uploading anything else: an upload that lands on the restart
  left the ESP32 failing USB enumeration until a power cycle.
- `capture-supervisor.ps1` restarts the serial capture whenever the board is present and
  nothing is logging it, so a replug or bridge restart no longer ends the log.

## Risks that stay after it works

- A failed handshake between the ESP32 and the RA4M1's bootloader leaves the RA4M1 in its ROM
  bootloader until a power cycle. That lives in the bridge firmware.
- Anything that wedges the ESP32 can only be cleared by a power cycle. An RA4M1 watchdog
  resets the RA4M1, not the ESP32.
- On USB at the home PC none of this can strand the board: the ESP32 serves USB and drives
  the RA4M1's bootloader itself. In the field it means a visit.
