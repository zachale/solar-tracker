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

// Over-the-air updates that undo themselves when they go wrong.
//
// The manifest in Cloud Storage is one line, "<version> <bytes> <crc32>", naming the image
// that should be running and describing it exactly. The ESP32 downloads the image into its
// own flash and reprograms the RA4M1 from there; the RA4M1's flash is not touched until the
// image is whole, its transfer CRC checks out, and it has been read back and matches the
// manifest's size and CRC.
//
// Every install is a trial. Before one starts, the SD card must hold a copy of the running
// image, written straight from the RA4M1's flash and read back identical to it. The new image
// then has to prove it can still be updated, by fetching the manifest after it has run for a
// few minutes. Until it does:
//   - booting too many times without confirming (a crash loop, or the watchdog firing),
//   - running past the confirmation deadline without reaching the manifest,
//   - coming up without a working watchdog, or
//   - the manifest being pointed back at the old version (an operator abort)
// makes it write the saved copy back to the ESP32, read it back, and flash it. No download is
// needed to undo. A version that fails its trial is never installed again; a fix ships under
// a new name.
//
// The ESP32 only ever holds one image, and only on its way to the RA4M1. Its SPIFFS loses
// space for good whenever a write fails on a full filesystem, the bridge has no command to
// check or format it, and it deletes an image right after flashing it anyway. So the copy
// that has to last lives on the SD card, and the ESP32's filesystem is cleared before every
// image goes onto it.
//
// State lives in the RA4M1's data flash, which reflashing leaves alone, as two records in
// separate erase blocks, each written only when something changed: a power cut mid-write
// loses at most the newest change, never the last good one.
class OtaUpdater
{
public:
  // First thing in setup() after the watchdog. Does the trial bookkeeping, and rolls back on
  // the spot if this image has booted too often without confirming or has no watchdog. That
  // can happen before the rest of setup() has started the SD card, so it is given the pin.
  void boot(uint8_t sdChipSelect);

  // Every loop pass. Returns at once unless a check is due, which costs one short HTTPS GET.
  // `radioFree` must be false while the actuator is moving, in SAFE, or with the settings
  // switch on; nothing here touches the radio then, and a trial's clock does not run.
  void service(WifiClient &wifi, bool radioFree);

  // True while a download is in flight. Anything that could reset the radio, the sync's
  // WiFi.end() in particular, must wait until this is false or it kills the download.
  bool busy() const;

private:
  static const uint8_t VERSION_LEN = 16; // 15 characters and the terminator

  enum Phase : uint8_t
  {
    IDLE = 0,
    TRIAL = 1,
  };

  struct State
  {
    uint32_t magic;
    uint32_t generation;       // which of the two records is newer
    uint8_t phase;
    uint8_t copySaved;         // the SD card holds `good`, read back identical to it when saved
    uint8_t trialBoots;        // boots of `trial` without a confirmation
    uint8_t rollbackRequested; // the running trial asked for the copy of `good` to be flashed
    uint8_t installAttempts;   // installs of `trial` that never booted as `trial`
    uint8_t attempts;          // downloads of `attempted` started and not installed
    uint8_t reserved[2];
    uint32_t copySize;          // the copy's length and CRC-32, for checking it while `good`
    uint32_t copyCrc;           // is no longer the image running
    char good[VERSION_LEN];     // the version last confirmed good
    char trial[VERSION_LEN];    // the version on trial
    char rejected[VERSION_LEN];  // the last version that failed its trial; never retried
    char attempted[VERSION_LEN]; // the last version a download was started for
    uint32_t checksum;
  };

  State state;
  State saved; // what is in data flash now, so unchanged state is never rewritten
  uint8_t record = 0;
  OTAUpdate ota;
  bool dev = false;
  uint8_t sdChipSelect = 0;

  bool firstCheckDone = false;
  bool checkSoon = false;
  unsigned long lastCheckAt = 0;
  bool copyTried = false;
  unsigned long lastCopyAt = 0;

  // A trial's deadline counts only time in which it could have confirmed: with the switch on
  // or the tracker in SAFE it cannot reach the network, and that is not its fault.
  unsigned long trialActiveMs = 0;
  unsigned long lastServiceAt = 0;

  bool rollbackTried = false;
  unsigned long lastRollbackAt = 0;

  bool downloading = false;
  String target;
  uint32_t targetSize = 0; // from the manifest
  uint32_t targetCrc = 0;
  int totalBytes = 0;
  int lastBytes = 0;
  unsigned long startedAt = 0;
  unsigned long lastPollAt = 0;
  unsigned long lastLogAt = 0;
  unsigned long lastAdvanceAt = 0;
  // Loop passes that ran while the download was in flight: the direct evidence that the
  // tracker kept working instead of waiting on the transfer.
  unsigned long loopsWhileDownloading = 0;

  bool checkDue() const;
  void check(WifiClient &wifi);
  void checkTrialDeadline();
  void confirm();
  void rollback(const String &reason);
  void startDownload(WifiClient &wifi, const String &version);
  void poll();
  void finish();
  void install();
  void fail(const String &step, int code, bool bridgeTouched = true);
  void restartBridge();

  bool saveCopy();
  bool copyMatchesRunning();
  bool readCopy(uint32_t expectedSize, uint32_t &crc, const uint8_t *compareTo);
  bool restoreCopy();
  bool clearEsp32();
  bool readImage(uint32_t &size, uint32_t &crc);
  bool newImageMatchesManifest();

  void load();
  void save();
  static bool recordValid(const State &s);
  static uint32_t checksumOf(const State &s);
  static void copyVersion(char *dst, const String &src);
  static bool validVersion(const String &version);
  static bool parseManifest(const String &body, String &version, uint32_t &size, uint32_t &crc);
  String describe() const;
};

#endif
