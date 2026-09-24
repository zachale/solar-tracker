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
// that should be running and describing it exactly. The ESP32 downloads images into one of
// two slots in its own flash and reprograms the RA4M1 from there; the RA4M1's flash is not
// touched until an image is whole, its transfer CRC checks out, and it has been read back
// and matches the manifest's size and CRC.
//
// Every install is a trial. Before one starts, the other slot must hold a copy of the running
// image, read back and compared byte for byte with the running flash. The new image then has
// to prove it can still be updated, by fetching the manifest after it has run for a few
// minutes. Until it does:
//   - booting too many times without confirming (a crash loop, or the watchdog firing),
//   - running past the confirmation deadline without reaching the manifest,
//   - coming up without a working watchdog, or
//   - the manifest being pointed back at the old version (an operator abort)
// makes it flash the saved copy back. No download is needed to undo. A version that fails its
// trial is never installed again; a fix ships under a new name.
//
// The bridge deletes an image file right after flashing it (BossaUnoR4WiFI.cpp), so every
// install and every rollback consumes a slot. The tracker downloads its own image again
// whenever that leaves it without a rollback copy, and starts no trial until it has one.
//
// State lives in the RA4M1's data flash, which reflashing leaves alone, as two records in
// separate erase blocks, each written only when something changed: a power cut mid-write
// loses at most the newest change, never the last good one.
class OtaUpdater
{
public:
  // First thing in setup() after the watchdog. Does the trial bookkeeping, and rolls back on
  // the spot if this image has booted too often without confirming or has no watchdog.
  void boot();

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
    uint8_t goodSlot;          // which slot the rollback copy is in
    uint8_t goodSlotValid;     // that slot holds `good`, read back identical when saved
    uint8_t trialSlot;         // where the trial image was downloaded (consumed on install)
    uint8_t trialBoots;        // boots of `trial` without a confirmation
    uint8_t rollbackRequested; // the running trial asked for the copy of `good` to be flashed
    uint8_t installAttempts;   // installs of `trial` that never booted as `trial`
    uint8_t reserved;
    char good[VERSION_LEN];     // the version last confirmed good
    char trial[VERSION_LEN];    // the version on trial
    char rejected[VERSION_LEN]; // the last version that failed its trial; never retried
    uint32_t checksum;
  };

  enum Job : uint8_t
  {
    NO_JOB,
    SEED,    // downloading the running version as its own rollback copy
    INSTALL, // downloading a new version to try
  };

  State state;
  State saved; // what is in data flash now, so unchanged state is never rewritten
  uint8_t record = 0;
  OTAUpdate ota;
  bool dev = false;

  bool firstCheckDone = false;
  bool checkSoon = false;
  unsigned long lastCheckAt = 0;

  // A trial's deadline counts only time in which it could have confirmed: with the switch on
  // or the tracker in SAFE it cannot reach the network, and that is not its fault.
  unsigned long trialActiveMs = 0;
  unsigned long lastServiceAt = 0;

  bool rollbackTried = false;
  unsigned long lastRollbackAt = 0;

  Job job = NO_JOB;
  String target;
  uint8_t targetSlot = 0;
  uint32_t targetSize = 0; // from the manifest, for INSTALL
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

  // Downloads that keep failing are not repeated forever. Kept in RAM on purpose: a restart
  // gives a version another three tries, a failed trial rejects it for good.
  String failedVersion;
  int failedAttempts = 0;

  bool checkDue() const;
  void check(WifiClient &wifi);
  void checkTrialDeadline();
  void confirm();
  void rollback(const String &reason);
  void startDownload(Job what, const String &version, uint8_t slot);
  void poll();
  void finish();
  void install();
  void fail(const String &step, int code);

  bool readSlot(uint8_t slot, uint32_t &size, uint32_t &crc, const uint8_t *compareTo);
  bool copyMatchesRunning(uint8_t slot);
  bool newImageMatchesManifest(uint8_t slot);

  void load();
  void save();
  static bool recordValid(const State &s);
  static uint32_t checksumOf(const State &s);
  static const char *slotPath(uint8_t slot);
  static uint8_t otherSlot(uint8_t slot) { return slot ? 0 : 1; }
  static void copyVersion(char *dst, const String &src);
  static bool validVersion(const String &version);
  static bool parseManifest(const String &body, String &version, uint32_t &size, uint32_t &crc);
  String describe() const;
};

#endif
