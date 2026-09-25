#!/usr/bin/env bash
# Build a versioned firmware image and package it as an .ota for the UNO R4 WiFi.
#
#   tools/ota/build.sh v2
#
# Produces tools/ota/out/<version>/ containing:
#   solar-tracker.ino.bin   what a USB upload flashes
#   <version>.ota           what the ESP32 downloads over the air
#
# The .ota is Arduino's format: LZSS-compressed image behind a 20 byte header (length,
# CRC32, magic 0x23411002, version field with the compression flag). lzss.py, lzss.c and
# bin2ota.py are Arduino's own tools from ArduinoIoTCloud/extras/tools, unmodified; the
# pipeline was checked by decoding Arduino's reference UNOR4WIFI_Animation.ota and
# re-encoding it byte-identical. Every build here is decoded again and compared against
# the .bin before it is allowed to count.
set -euo pipefail

# Test options, for tools/ota/verify.sh. Neither belongs in an image for the field.
#   --fast          compress OtaUpdater's timers (check every 60 s, confirm after 60 s,
#                   roll back after 300 s) so the fault matrix runs in minutes
#   --fault=<name>  build a deliberately broken image: hang-setup, crash-setup, no-confirm
EXTRA=""
while [[ "${1:-}" == --* ]]; do
  case "$1" in
    --fast) EXTRA="$EXTRA -DOTA_FAST_TIMING" ;;
    --fault=hang-setup) EXTRA="$EXTRA -DOTA_FAULT_HANG_SETUP" ;;
    --fault=crash-setup) EXTRA="$EXTRA -DOTA_FAULT_CRASH_SETUP" ;;
    --fault=no-confirm) EXTRA="$EXTRA -DOTA_FAULT_NO_CONFIRM" ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
  shift
done

VERSION="${1:-}"
# 15 characters at most: OtaUpdater keeps versions in 16 byte fields in data flash.
if [[ ! "$VERSION" =~ ^[A-Za-z][A-Za-z0-9._-]{0,14}$ ]]; then
  echo "usage: $0 [--fast] [--fault=<name>] <version>   (a letter, then up to 14 of [A-Za-z0-9._-])" >&2
  exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/out/$VERSION"
PY="$HERE/.venv/bin/python"

# The tracker asks for this image with one AT command, and WiFiS3 sends at most 127
# characters of any command (Modem.h MAX_BUFF_SIZE 128, formatted with vsnprintf). A longer
# one loses its newline and the bridge never runs it. OtaUpdater refuses such a URL at run
# time as well; this catches it before anything is built or published.
BASE="$(sed -n 's/^#define OTA_BASE_URL "\(.*\)"$/\1/p' "$ROOT/secrets.h")"
[[ -n "$BASE" ]] || { echo "no OTA_BASE_URL in secrets.h" >&2; exit 1; }
LINE="AT+OTADOWNLOADSTART=$BASE/$VERSION.ota,/a.bin"
if (( ${#LINE} + 2 > 127 )); then
  echo "FAIL: '$VERSION' makes the download command $(( ${#LINE} + 2 )) bytes; the modem sends at most 127" >&2
  exit 1
fi

if [[ ! -x "$PY" ]]; then
  python3 -m venv "$HERE/.venv"
  "$PY" -m pip install -q crccheck
fi
if [[ ! -f "$HERE/lzss.dylib" && ! -f "$HERE/lzss.so" ]]; then
  ext=so; [[ "$(uname)" == Darwin ]] && ext=dylib
  cc -O2 -shared -fPIC -o "$HERE/lzss.$ext" "$HERE/lzss.c"
fi

rm -rf "$OUT"
mkdir -p "$OUT/build"

# The version goes in as a bare token and OtaUpdater.h stringifies it, so no quoting has
# to survive arduino-cli's own argument splitting.
arduino-cli compile -b arduino:renesas_uno:unor4wifi \
  --build-path "$OUT/build" --output-dir "$OUT" \
  --build-property "compiler.cpp.extra_flags=-DFIRMWARE_VERSION_TOKEN=$VERSION$EXTRA" \
  "$ROOT" | grep -E "bytes"

BIN="$OUT/solar-tracker.ino.bin"
# The image passes through the ESP32's SPIFFS on its way to the RA4M1, one at a time. SPIFFS
# loses space for good on every write that fails for lack of it, and the bench unit's 512 KB
# partition was left holding about 190 KB by the tests that found that out
# (bench/18_slot_stress, bench/19_spiffs_reclaim). 168 KB fits there with room to spare; a
# download that ran out would be truncated silently by the bridge and cost more space.
if (( $(wc -c < "$BIN") > 172032 )); then
  echo "FAIL: $(wc -c < "$BIN") byte image; above 168 KB it may not fit on the bench unit's ESP32" >&2
  exit 1
fi
# OtaUpdater works out the running image's length from these linker symbols and demands the
# rollback copy be exactly that long. If a toolchain change ever adds another section to the
# .bin, this is where it shows, rather than as a board that refuses every rollback copy.
NM="$(ls ~/Library/Arduino15/packages/arduino/tools/arm-none-eabi-gcc/*/bin/arm-none-eabi-nm | head -1)"
FORMULA="$("$NM" "$OUT/build/"*.elf | python3 -c '
import sys
sym = {p[2]: int(p[0], 16) for p in (l.split() for l in sys.stdin) if len(p) == 3}
print((sym["__etext"] - sym["__ROM_Start"]) + (sym["__data_end__"] - sym["__data_start__"]))' 2>/dev/null)"
if [[ "$FORMULA" != "$(wc -c < "$BIN" | tr -d ' ')" ]]; then
  echo "FAIL: linker formula gives ${FORMULA:-nothing}, the image is $(wc -c < "$BIN" | tr -d ' ') bytes; OtaUpdater's read-back would reject every copy" >&2
  exit 1
fi
if ! grep -qa "Firmware version: $VERSION" "$BIN"; then
  echo "FAIL: the image does not carry 'Firmware version: $VERSION'" >&2
  exit 1
fi

# lzss.py loads ./lzss.<ext>, so it has to run from this directory.
(
  cd "$HERE"
  "$PY" lzss.py --encode "$BIN" "$OUT/$VERSION.lzss" >/dev/null
  "$PY" bin2ota.py UNOR4WIFI "$OUT/$VERSION.lzss" "$OUT/$VERSION.ota"
  tail -c +21 "$OUT/$VERSION.ota" > "$OUT/roundtrip.lzss"
  "$PY" lzss.py --decode "$OUT/roundtrip.lzss" "$OUT/roundtrip.bin" >/dev/null
)

if ! cmp -s "$BIN" "$OUT/roundtrip.bin"; then
  echo "FAIL: decoding $VERSION.ota does not give back the .bin" >&2
  exit 1
fi

"$PY" - "$OUT/$VERSION.ota" <<'PY'
import sys, struct, zlib
d = open(sys.argv[1], 'rb').read()
length, crc = struct.unpack('<II', d[:8])
body = d[8:]
assert length == len(body), "length field"
assert crc == zlib.crc32(body), "crc32"
assert struct.unpack('<I', body[:4])[0] == 0x23411002, "magic"
assert body[11] == 0x40, "compression flag"
PY

rm -f "$OUT/roundtrip.lzss" "$OUT/roundtrip.bin" "$OUT/$VERSION.lzss"
[[ -n "$EXTRA" ]] && echo "TEST IMAGE:$EXTRA" > "$OUT/TEST-IMAGE"
echo "built $VERSION${EXTRA:+ (test:$EXTRA)}: $(wc -c < "$BIN" | tr -d ' ') byte image, $(wc -c < "$OUT/$VERSION.ota" | tr -d ' ') byte .ota (round trip, CRC, magic checked)"
