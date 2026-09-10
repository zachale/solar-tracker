#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

// Spools one CSV row per sampling window onto the SD card so the readings can be
// uploaded in a single nightly request instead of a request per reading.
//
// Every entry point is failure-tolerant on purpose. Telemetry is the least
// important job this firmware has, and the device cannot be serviced remotely,
// so a missing card, a full card or a failed write must never do anything worse
// than lose a row.
class Telemetry
{
public:
  // Eight-three name: the SD library is built on the legacy short-name FAT API.
  static constexpr const char *logFile = "log.csv";

  // The whole spool is uploaded in one request, so it has to fit in RAM
  // alongside everything else. At roughly 36 bytes per row and one row every
  // ten minutes this holds about a day and a half of readings; past that the
  // oldest data is dropped rather than risk exhausting the heap.
  static constexpr size_t maxBytes = 8192;

  static String formatRow(uint32_t timestamp,
                          float temperature,
                          int extendedPercent,
                          const String &status,
                          int windPeak,
                          bool settingsServer,
                          bool windSensor,
                          bool away);

  static bool append(const String &row);
  static String readAll();
  static void clear();
  static size_t size();
};

#endif
