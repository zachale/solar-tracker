#!/usr/bin/env bash
# Build and run the native test suite.
#
# The firmware's own .cpp files are compiled here against the stubs in test/stubs,
# so these tests exercise the real shipping logic rather than a copy of it.
#
# Usage:  ./test/run.sh [doctest args, e.g. -ts=schedule]

set -euo pipefail
cd "$(dirname "$0")/.."

ARDUINOJSON="$HOME/Documents/Arduino/libraries/ArduinoJson/src"

if [ ! -f secrets.h ]; then
  echo "secrets.h is missing from the repository root. src/SolarTracker/SolarTracker.cpp" >&2
  echo "includes it (the API base URL), here as in the firmware build." >&2
  exit 1
fi
OUT=".test-build"
mkdir -p "$OUT"

SOURCES=(
  test/test_main.cpp
  test/test_clock.cpp
  test/test_wind.cpp
  test/test_actuator.cpp
  test/test_timing.cpp
  test/test_settings.cpp
  test/test_urldecode.cpp
  test/test_telemetry.cpp
  test/test_handoff.cpp
  test/test_tracker.cpp
  test/stubs/mock.cpp
  test/stubs/tracker_deps.cpp
  src/Clock/Clock.cpp
  src/WindSpeedSensor/WindSpeedSensor.cpp
  src/LinearActuator/LinearActuator.cpp
  src/ButtonPanel/ButtonPanel.cpp
  src/SettingsManager/SettingsManager.cpp
  src/Telemetry/Telemetry.cpp
  src/PositionHandoff/PositionHandoff.cpp
  src/SolarTracker/SolarTracker.cpp
)

c++ -std=c++17 -g -O0 \
    -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 \
    -DARDUINOJSON_ENABLE_STD_STRING=1 \
    -I test -I test/stubs -I "$ARDUINOJSON" \
    -Wall -Wextra -Wno-unused-parameter \
    "${SOURCES[@]}" -o "$OUT/tests"

"$OUT/tests" "$@"
