#!/usr/bin/env bash
# Publish a built .ota and point the tracker at it.
#
#   tools/ota/publish.sh v2
#
# Order matters: the image is uploaded and read back byte-for-byte before the manifest
# changes, so a tracker never sees a version it cannot fetch. Images are never
# overwritten; a version name is used once.
#
# Only ever publish an image that has already been flashed over USB and seen to boot and
# check the manifest. The ESP32 verifies the file's CRC, not whether the sketch in it
# runs, and a sketch that does not boot needs someone at the board.
set -euo pipefail

# --no-manifest  upload and verify the image, leave the manifest alone
# --test         allow an image build.sh marked as a test image (fast timers, injected faults)
MANIFEST=1
ALLOW_TEST=0
while [[ "${1:-}" == --* ]]; do
  case "$1" in
    --no-manifest) MANIFEST=0 ;;
    --test) ALLOW_TEST=1 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
  shift
done

VERSION="${1:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OTA="$HERE/out/$VERSION/$VERSION.ota"
BUCKET="solar-tracker-442903-ota"
TOKEN="$(cat "$HERE/.ota_token")"
BASE="https://storage.googleapis.com/$BUCKET/$TOKEN"

[[ -f "$OTA" ]] || { echo "no $OTA; run build.sh $VERSION first" >&2; exit 1; }
if [[ -f "$HERE/out/$VERSION/TEST-IMAGE" && $ALLOW_TEST -eq 0 ]]; then
  echo "REFUSED: $VERSION is a test image ($(cat "$HERE/out/$VERSION/TEST-IMAGE")); pass --test if that is intended" >&2
  exit 1
fi

# gcloud reports every copy on stderr with the full gs:// path, token included. Its output
# is kept and shown only on failure, with the token masked, because anything this script
# prints can end up in a terminal log or a transcript.
quiet() {
  local out
  if ! out="$("$@" 2>&1)"; then
    printf '%s\n' "${out//$TOKEN/<token>}" >&2
    return 1
  fi
}

if curl -fsS -o /dev/null "$BASE/$VERSION.ota" 2>/dev/null; then
  echo "$VERSION.ota is already published; checking it matches the local build"
else
  quiet gcloud storage cp "$OTA" "gs://$BUCKET/$TOKEN/$VERSION.ota" --content-type=application/octet-stream --quiet
fi
if ! curl -fsS "$BASE/$VERSION.ota" | cmp -s - "$OTA"; then
  echo "FAIL: published $VERSION.ota differs from the local build; manifest left alone" >&2
  exit 1
fi

if [[ $MANIFEST -eq 0 ]]; then
  echo "uploaded $VERSION ($(wc -c < "$OTA" | tr -d ' ') bytes); manifest left alone"
  exit 0
fi

# "<version> <bytes> <crc32>" of the decompressed image: the tracker reads the downloaded
# file back and installs it only if both match.
LINE="$(python3 -c 'import sys,zlib; d=open(sys.argv[2],"rb").read(); print(sys.argv[1], len(d), format(zlib.crc32(d),"08x"))' "$VERSION" "$HERE/out/$VERSION/solar-tracker.ino.bin")"
printf '%s\n' "$LINE" > "$HERE/out/manifest.txt"
quiet gcloud storage cp "$HERE/out/manifest.txt" "gs://$BUCKET/$TOKEN/manifest.txt" \
  --cache-control=no-store --content-type=text/plain --quiet
served="$(curl -fsS "$BASE/manifest.txt")"
if [[ "$served" != "$LINE" ]]; then
  echo "FAIL: manifest serves '$served', expected '$LINE'" >&2
  exit 1
fi
echo "published $VERSION ($(wc -c < "$OTA" | tr -d ' ') bytes); manifest now serves '$served'"
