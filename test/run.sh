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
OUT=".test-build"
mkdir -p "$OUT"

SOURCES=(
  test/test_main.cpp
  test/test_clock.cpp
  test/test_wind.cpp
  test/test_actuator.cpp
  test/test_settings.cpp
  test/stubs/mock.cpp
  src/Clock/Clock.cpp
  src/WindSpeedSensor/WindSpeedSensor.cpp
  src/LinearActuator/LinearActuator.cpp
  src/ButtonPanel/ButtonPanel.cpp
  src/SettingsManager/SettingsManager.cpp
)

c++ -std=c++17 -g -O0 \
    -DARDUINOJSON_ENABLE_ARDUINO_STRING=1 \
    -DARDUINOJSON_ENABLE_STD_STRING=1 \
    -I test -I test/stubs -I "$ARDUINOJSON" \
    -Wall -Wextra -Wno-unused-parameter \
    "${SOURCES[@]}" -o "$OUT/tests"

"$OUT/tests" "$@"
