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

VERSION="${1:-}"
if [[ ! "$VERSION" =~ ^[A-Za-z][A-Za-z0-9._-]{0,31}$ ]]; then
  echo "usage: $0 <version>   (a letter, then up to 31 of [A-Za-z0-9._-]; e.g. v2)" >&2
  exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/out/$VERSION"
PY="$HERE/.venv/bin/python"

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
  --build-property "compiler.cpp.extra_flags=-DFIRMWARE_VERSION_TOKEN=$VERSION" \
  "$ROOT" | grep -E "bytes"

BIN="$OUT/solar-tracker.ino.bin"
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
echo "built $VERSION: $(wc -c < "$BIN" | tr -d ' ') byte image, $(wc -c < "$OUT/$VERSION.ota" | tr -d ' ') byte .ota (round trip, CRC, magic checked)"
