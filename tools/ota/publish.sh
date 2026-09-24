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

VERSION="${1:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OTA="$HERE/out/$VERSION/$VERSION.ota"
BUCKET="solar-tracker-442903-ota"
TOKEN="$(cat "$HERE/.ota_token")"
BASE="https://storage.googleapis.com/$BUCKET/$TOKEN"

[[ -f "$OTA" ]] || { echo "no $OTA; run build.sh $VERSION first" >&2; exit 1; }

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

printf '%s\n' "$VERSION" > "$HERE/out/manifest.txt"
quiet gcloud storage cp "$HERE/out/manifest.txt" "gs://$BUCKET/$TOKEN/manifest.txt" \
  --cache-control=no-store --content-type=text/plain --quiet
served="$(curl -fsS "$BASE/manifest.txt")"
if [[ "$served" != "$VERSION" ]]; then
  echo "FAIL: manifest serves '$served', expected '$VERSION'" >&2
  exit 1
fi
echo "published $VERSION ($(wc -c < "$OTA" | tr -d ' ') bytes); manifest now serves '$served'"
