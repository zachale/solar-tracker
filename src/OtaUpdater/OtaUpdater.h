#ifndef OTA_UPDATER_H
#define OTA_UPDATER_H

#include <Arduino.h>
#include <OTAUpdate.h>
#include "../WifiClient/WifiClient.h"

// The running build's version. tools/ota/build.sh passes it as a bare token
// (-DFIRMWARE_VERSION_TOKEN=v2) and it is stringified here, which sidesteps quoting a
// string literal through arduino-cli's build properties. Anything built another way,
// the IDE included, is "dev" and never updates itself.
#define OTA_STRINGIFY_(x) #x
#define OTA_STRINGIFY(x) OTA_STRINGIFY_(x)
#ifdef FIRMWARE_VERSION_TOKEN
#define FIRMWARE_VERSION OTA_STRINGIFY(FIRMWARE_VERSION_TOKEN)
#else
#define FIRMWARE_VERSION "dev"
#endif

// Pulls new firmware over the air without stopping the tracker.
//
// A one-line manifest in Cloud Storage names the version that should be running. When
// it differs from FIRMWARE_VERSION, the ESP32 is told to fetch <OTA_BASE_URL>/<version>.ota
// into its own flash, and from then on the tracker only polls the download's progress
// between its normal work. The image is staged on the ESP32, never in the RA4M1's flash,
// so the 122,880 byte InternalStorage ceiling does not apply. Once the whole file is down
// and its CRC checks out, the ESP32 reprograms the RA4M1 and resets it into the new build.
//
// Non-blocking needs bridge firmware 0.5.0 or later; this unit runs 0.6.0.
class OtaUpdater
{
public:
  // Call on every loop pass. Returns at once unless a manifest check is due, which costs
  // one short HTTPS GET. `radioFree` must be false while the actuator is moving or the
  // tracker is in SAFE; nothing here touches the radio then.
  void service(WifiClient &wifi, bool radioFree);

  // True while a download is in flight. Anything that could reset the radio, the sync's
  // WiFi.end() in particular, must wait until this is false or it kills the download.
  bool busy() const;

private:
  enum State
  {
    IDLE,
    DOWNLOADING
  };

  State state = IDLE;
  OTAUpdate ota;

  bool firstCheckDone = false;
  unsigned long lastCheckAt = 0;

  String target;
  int totalBytes = 0;
  int lastBytes = 0;
  unsigned long startedAt = 0;
  unsigned long lastPollAt = 0;
  unsigned long lastLogAt = 0;
  unsigned long lastAdvanceAt = 0;
  // Loop passes that ran while the download was in flight. This is the direct evidence
  // that the tracker kept working instead of waiting on the transfer.
  unsigned long loopsWhileDownloading = 0;

  // A version that keeps failing is not fetched forever: after a few attempts it is
  // left alone until the manifest names something else or the board restarts.
  String failedVersion;
  int failedAttempts = 0;

  bool checkDue() const;
  void check(WifiClient &wifi);
  void start(const String &version);
  void poll();
  void finish();
  void fail(const String &step, int code);
  static bool validVersion(const String &version);
};

#endif
