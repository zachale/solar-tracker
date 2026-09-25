#include "OtaUpdater.h"
#include <EEPROM.h>
#include <SD.h>
#include <stddef.h>
#include "../../secrets.h"
#include "../Watchdog/Watchdog.h"

namespace
{
#include "root_ca.h"

// OTA_BASE_URL lives in the gitignored secrets.h, e.g.
//   #define OTA_BASE_URL "https://storage.googleapis.com/<bucket>/<token>"
// The token in that path is the only thing keeping the images unlisted, and this repo is
// public, so it must never be printed: serial captures end up committed.

const uint32_t STATE_MAGIC = 0x4F544135; // "OTA5"
// Two records in separate 1 KB erase blocks of the 8 KB data flash. The virtual EEPROM
// rewrites a whole block per write, so records sharing a block would be lost together.
const int RECORD_ADDRESS[2] = {0, 1024};

// Test builds compress the timers so the fault matrix runs in minutes rather than hours.
// The logic is the same; only these numbers change. tools/ota/build.sh --fast sets it.
#ifdef OTA_FAST_TIMING
const unsigned long CHECK_INTERVAL_MS = 60000UL;
const unsigned long CONFIRM_MIN_UPTIME_MS = 60000UL;
const unsigned long CONFIRM_DEADLINE_MS = 300000UL;
#else
const unsigned long CHECK_INTERVAL_MS = 300000UL;     // five minutes
const unsigned long CONFIRM_MIN_UPTIME_MS = 180000UL; // a trial must survive three minutes
const unsigned long CONFIRM_DEADLINE_MS = 900000UL;   // and confirm within fifteen
#endif
const unsigned long FIRST_CHECK_DELAY_MS = 30000UL; // after boot, once WiFi has settled
const unsigned long SOON_MS = 10000UL;              // follow-up check after a confirm
const unsigned long ROLLBACK_RETRY_MS = 60000UL;
const unsigned long COPY_RETRY_MS = 600000UL; // a rollback copy that could not be saved
const uint8_t MAX_TRIAL_BOOTS = 3;
const uint8_t MAX_INSTALL_ATTEMPTS = 3;
const int MAX_DOWNLOAD_ATTEMPTS = 3;

const unsigned long POLL_INTERVAL_MS = 250UL; // each poll is one AT round trip
const unsigned long LOG_INTERVAL_MS = 2000UL;
const unsigned long STALL_TIMEOUT_MS = 60000UL;
const int READ_CHUNK = 1023;
// The bridge takes a file write's length as a uint8_t (cmds_esp_generic.h).
const size_t APPEND_CHUNK = 255;

// The one image the ESP32 holds, on its way to the RA4M1. /b.bin and /update.bin are where
// earlier builds and Arduino's examples put images; they are deleted whenever this is written
// so nothing else ever takes up the space.
const char IMAGE_PATH[] = "/a.bin";
const char *const STALE_PATHS[] = {"/b.bin", "/update.bin"};
// The rollback copy, on the SD card. 8.3, which is all the SD library takes.
const char COPY_FILE[] = "OTA.BIN";

// The running sketch in code flash, from the linker script. It starts at __ROM_Start (0x4000
// on this board; not SCB->VTOR, which main.cpp points at a copy of the vector table in RAM)
// and the .bin every flash writes is the code up to __etext followed by the initial values of
// .data. tools/ota/build.sh checks this formula against every image it builds.
extern "C" const uint8_t __ROM_Start[], __etext[], __data_start__[], __data_end__[];

uint32_t runningImageSize()
{
  return (uint32_t)(__etext - __ROM_Start) + (uint32_t)(__data_end__ - __data_start__);
}

uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len)
{
  // zlib's CRC-32, bit by bit: a whole image costs about 50 ms and no table.
  crc = ~crc;
  while (len--)
  {
    crc ^= *data++;
    for (int k = 0; k < 8; k++)
    {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
  return ~crc;
}

bool mountEsp32()
{
  // 0 is format-on-fault off: this only ever mounts. The download and flash commands mount
  // and unmount on their own; the file commands assume the filesystem is already mounted.
  std::string res;
  return modem.write(std::string(PROMPT(_MOUNTFS)), res, "%s%d\r\n", CMD_WRITE(_MOUNTFS), 0);
}
} // namespace

// ---------------------------------------------------------------------------------------------
// Boot

void OtaUpdater::boot(uint8_t sdChipSelect)
{
  this->sdChipSelect = sdChipSelect;
  const String running = FIRMWARE_VERSION;
  if (running == "dev")
  {
    dev = true;
    Serial.println("OTA: dev build, updates disabled");
    return;
  }

  load();
  Serial.println("OTA: " + describe());

  if (state.phase == TRIAL)
  {
    if (running == state.trial)
    {
      state.trialBoots++;
      // It got here, so its download worked; from now on the trial does the counting.
      state.attempted[0] = '\0';
      state.attempts = 0;
      save();
      Serial.println("OTA: trial boot " + String(state.trialBoots) + " of " + String(MAX_TRIAL_BOOTS) + " for " +
                     running + ", last reset: " + Watchdog::resetCauseText());
      if (!Watchdog::armed())
      {
        rollback("this image could not arm its watchdog");
      }
      else if (state.trialBoots > MAX_TRIAL_BOOTS)
      {
        rollback("booted " + String(state.trialBoots) + " times without confirming");
      }
      return;
    }

    if (running == state.good)
    {
      if (state.rollbackRequested)
      {
        // The copy on the SD card is untouched by the rollback, so it still stands.
        Serial.println("OTA: ROLLED BACK from " + String(state.trial) + " to " + running + "; " +
                       String(state.trial) + " is rejected and will not be installed again");
        copyVersion(state.rejected, state.trial);
        state.trial[0] = '\0';
      }
      else
      {
        // The trial's name is kept, so the next install() of the same version counts as
        // another attempt rather than a first one.
        state.installAttempts++;
        Serial.println("OTA: install of " + String(state.trial) + " did not take, attempt " +
                       String(state.installAttempts) + " of " + String(MAX_INSTALL_ATTEMPTS));
        if (state.installAttempts >= MAX_INSTALL_ATTEMPTS)
        {
          copyVersion(state.rejected, state.trial);
        }
      }
      state.phase = IDLE;
      state.trialBoots = 0;
      state.rollbackRequested = 0;
      save();
      return;
    }

    // Neither: something else was flashed over USB during a trial.
    Serial.println("OTA: running " + running + ", neither the trial " + String(state.trial) + " nor the good " +
                   String(state.good) + "; taking it as the new base");
    state.phase = IDLE;
    state.trial[0] = '\0';
    state.trialBoots = 0;
    state.rollbackRequested = 0;
  }

  if (running != state.good)
  {
    // A new base, usually a USB flash. The copy on the card is of some other version, so a
    // copy of this one is saved before any trial can start.
    copyVersion(state.good, running);
    state.copySaved = 0;
    if (running == state.rejected)
    {
      state.rejected[0] = '\0';
    }
    Serial.println("OTA: " + running + " is the new base; a copy of it goes on the SD card first");
  }
  save();
}

// ---------------------------------------------------------------------------------------------
// Loop

bool OtaUpdater::busy() const
{
  return downloading;
}

void OtaUpdater::service(WifiClient &wifi, bool radioFree)
{
  if (dev)
  {
    return;
  }

  unsigned long now = millis();
  if (state.phase == TRIAL && radioFree && lastServiceAt != 0)
  {
    trialActiveMs += now - lastServiceAt;
  }
  lastServiceAt = now;

  if (downloading)
  {
    loopsWhileDownloading++;
    // The ESP32 keeps downloading whether or not we ask how it is going, so skipping polls
    // while the actuator moves costs nothing but a later progress line.
    if (radioFree)
    {
      poll();
    }
    return;
  }

  if (!radioFree)
  {
    return;
  }

  // Every pass, not only at a check: a trial that has lost the network can never fetch the
  // manifest, and that is exactly the case the deadline exists for.
  checkTrialDeadline();

  // Local and quick, no radio: done as soon as there is a version to keep a copy of.
  if (state.phase == IDLE && !state.copySaved && (!copyTried || millis() - lastCopyAt >= COPY_RETRY_MS))
  {
    saveCopy();
    return;
  }

  if (!checkDue())
  {
    return;
  }
  firstCheckDone = true;
  checkSoon = false;
  lastCheckAt = millis();
  check(wifi);
}

bool OtaUpdater::checkDue() const
{
  if (!firstCheckDone)
  {
    return millis() >= FIRST_CHECK_DELAY_MS;
  }
  if (checkSoon)
  {
    return millis() - lastCheckAt >= SOON_MS;
  }
  return millis() - lastCheckAt >= CHECK_INTERVAL_MS;
}

void OtaUpdater::checkTrialDeadline()
{
  if (state.phase == TRIAL && String(FIRMWARE_VERSION) == state.trial && trialActiveMs > CONFIRM_DEADLINE_MS)
  {
    rollback("did not confirm within " + String(CONFIRM_DEADLINE_MS / 60000UL) + " minutes");
  }
}

void OtaUpdater::check(WifiClient &wifi)
{
  const String running = FIRMWARE_VERSION;
  String wanted;
  uint32_t wantedSize = 0, wantedCrc = 0;
  const bool manifestOk =
      parseManifest(wifi.get(String(OTA_BASE_URL) + "/manifest.txt"), wanted, wantedSize, wantedCrc);

  if (state.phase == TRIAL && running == state.trial)
  {
    if (manifestOk && wanted == state.good)
    {
      rollback("the manifest points back to " + wanted);
      return;
    }
#ifdef OTA_FAULT_NO_CONFIRM
    Serial.println("OTA: fault injection, never confirming");
    return;
#endif
    if (manifestOk && millis() >= CONFIRM_MIN_UPTIME_MS)
    {
      confirm();
      return;
    }
    Serial.println("OTA: " + running + " on trial, " + String(millis() / 1000UL) + " s up, manifest " +
                   (manifestOk ? "reachable" : "NOT reachable") + "; confirms after " +
                   String(CONFIRM_MIN_UPTIME_MS / 1000UL) + " s up, rolls back after " +
                   String(CONFIRM_DEADLINE_MS / 1000UL) + " s of trying");
    return;
  }

  if (!manifestOk)
  {
    Serial.println("OTA: no usable manifest, staying on " + running);
    return;
  }
  if (wanted == running)
  {
    Serial.println("OTA: up to date at " + running);
    return;
  }
  if (wanted == state.rejected)
  {
    Serial.println("OTA: manifest wants " + wanted + ", which failed its trial; staying on " + running);
    return;
  }
  if (wanted == state.attempted && state.attempts >= MAX_DOWNLOAD_ATTEMPTS)
  {
    Serial.println("OTA: " + wanted + " failed " + String(state.attempts) +
                   " times; not trying again until the manifest names another version");
    return;
  }
  if (!Watchdog::armed())
  {
    // Without a watchdog a new image that hangs is never reset, so it never rolls back.
    Serial.println("OTA: not updating to " + wanted + ": this image has no working watchdog");
    return;
  }
  // No trial may start without a verified copy of what is running now, and the copy is read
  // back and compared with the running image in full before every jump, not trusted from a flag.
  if (!state.copySaved)
  {
    Serial.println("OTA: manifest wants " + wanted + "; waiting for a copy of " + running + " on the SD card first");
    return;
  }
  if (!copyMatchesRunning())
  {
    Serial.println("OTA: the copy on the SD card no longer matches the running image; saving it again before any update");
    state.copySaved = 0;
    save();
    return;
  }

  Serial.println("OTA: manifest wants " + wanted + " (" + String(wantedSize) + " bytes), running " + running);
  targetSize = wantedSize;
  targetCrc = wantedCrc;
  startDownload(wifi, wanted);
}

void OtaUpdater::confirm()
{
  const String running = FIRMWARE_VERSION;
  Serial.println("OTA: " + running + " CONFIRMED after " + String(millis() / 1000UL) + " s and " +
                 String(state.trialBoots) + " boot(s); it is now the good version");
  copyVersion(state.good, running);
  // The card still holds the previous good version, not this one; the next pass replaces it.
  state.copySaved = 0;
  state.phase = IDLE;
  state.trial[0] = '\0';
  state.trialBoots = 0;
  state.rollbackRequested = 0;
  state.installAttempts = 0;
  save();
  copyTried = false;
  checkSoon = true;
}

// Only ever changes what is flashed; never what is believed to be good. An image is promoted
// by confirm() alone.
void OtaUpdater::rollback(const String &reason)
{
  if (rollbackTried && millis() - lastRollbackAt < ROLLBACK_RETRY_MS)
  {
    return;
  }
  rollbackTried = true;
  lastRollbackAt = millis();

  if (!state.copySaved)
  {
    // Cannot happen through this code: a trial starts only after the copy has been read back
    // intact, and nothing clears it during a trial. If it ever does, this image stays on
    // trial; if it is sound it can still confirm.
    Serial.println("OTA: CANNOT ROLL BACK (" + reason + "): no copy of " + String(state.good) + "; staying on trial");
    return;
  }

  Serial.println("OTA: ROLLING BACK to " + String(state.good) + " from the SD card: " + reason);
  if (!restoreCopy())
  {
    Serial.println("OTA: the rollback did not happen (the copy could not be put on the ESP32); trying again in " +
                   String(ROLLBACK_RETRY_MS / 1000UL) + " s");
    return;
  }

  state.rollbackRequested = 1;
  save();
  Serial.flush();
  delay(100);

  // On success the ESP32 flashes the copy and restarts the RA4M1: no return.
  int code = ota.update(IMAGE_PATH);

  // Still here, so nothing was flashed: the ESP32 did not answer, or could not open the file.
  // Nothing else changes, and the next boot or the next minute tries again.
  Serial.println("OTA: the rollback did not happen (" + String(code) + "); trying again in " +
                 String(ROLLBACK_RETRY_MS / 1000UL) + " s");
  state.rollbackRequested = 0;
  save();
}

// ---------------------------------------------------------------------------------------------
// Downloads

void OtaUpdater::startDownload(WifiClient &wifi, const String &version)
{
  String url = String(OTA_BASE_URL) + "/" + version + ".ota";
  target = version;
  downloading = true;

  // Counted before the attempt, in data flash. An attempt that ends with the board reset never
  // reaches fail(), and a count kept in RAM would start again after every reset: a download
  // that crashed the bridge and reset the RA4M1 was once retried every 50 s for as long as the
  // manifest named it.
  if (strcmp(state.attempted, version.c_str()) != 0)
  {
    copyVersion(state.attempted, version);
    state.attempts = 0;
  }
  state.attempts++;
  save();

  // WiFiS3 formats every AT command into a MAX_BUFF_SIZE byte buffer (128, Modem.h) with
  // vsnprintf and sends whatever fits. A longer command loses its closing newline: the
  // bridge never runs it, the RA4M1 waits out the 60 s timeout for an answer that is not
  // coming, and the unfinished line corrupts the next command too. That is what a 129 byte
  // startDownload did on this unit; see tools/ota/README.md.
  size_t line = strlen(CMD_WRITE(_OTA_DOWNLOAD_START)) + url.length() + 1 + strlen(IMAGE_PATH) + strlen(_ENDL);
  if (line >= MAX_BUFF_SIZE)
  {
    fail("URL too long: startDownload would be " + String(line) + " bytes, the modem sends at most " +
             String(MAX_BUFF_SIZE - 1),
         0, false);
    return;
  }

  // The bridge crashes on a download that fails, so an image that is not there is never
  // handed to it. This request goes through the RA4M1's own HTTP client instead.
  int status = wifi.probe(url);
  if (status != 200 && status != 206)
  {
    fail("probe of the image", status, false);
    return;
  }

  Serial.println("OTA: downloading " + version + " into " + IMAGE_PATH);
  if (!clearEsp32())
  {
    fail("clearing the ESP32 filesystem", 0);
    return;
  }
  int code = ota.setCACert(root_ca);
  if (code != OTAUpdate::OTA_ERROR_NONE)
  {
    fail("setCACert", code);
    return;
  }

  // startDownload() waits for the connection and the response headers, which is how it
  // learns the size; that part takes a second or two. The transfer itself runs on the ESP32
  // from here on, and the tracker carries on.
  unsigned long t0 = millis();
  int size = ota.startDownload(url.c_str(), IMAGE_PATH);
  Watchdog::progress();
  if (size <= 0)
  {
    fail("startDownload", size);
    return;
  }

  totalBytes = size;
  lastBytes = 0;
  loopsWhileDownloading = 0;
  startedAt = lastPollAt = lastLogAt = lastAdvanceAt = millis();
  Serial.println("OTA: downloading " + version + ", " + String(size) + " bytes, in the background (starting took " +
                 String(millis() - t0) + " ms)");
}

void OtaUpdater::poll()
{
  unsigned long now = millis();
  if (now - lastPollAt < POLL_INTERVAL_MS)
  {
    return;
  }
  lastPollAt = now;

  // Arduino's own example loops `while (downloadProgress() < size)`, which spins forever on
  // an error because every error code is negative. Errors are checked first here.
  int done = ota.downloadProgress();
  if (done < 0)
  {
    fail("downloadProgress", done);
    return;
  }
  if (done > lastBytes)
  {
    lastBytes = done;
    lastAdvanceAt = now;
  }
  if (done >= totalBytes)
  {
    finish();
    return;
  }
  if (now - lastAdvanceAt > STALL_TIMEOUT_MS)
  {
    fail("download stalled at " + String(done) + " bytes", 0);
    return;
  }
  if (now - lastLogAt >= LOG_INTERVAL_MS)
  {
    lastLogAt = now;
    Serial.println("OTA: " + String(done) + "/" + String(totalBytes) + " bytes (" +
                   String((long)done * 100L / totalBytes) + "%) after " + String((now - startedAt) / 1000UL) +
                   " s; loop ran " + String(loopsWhileDownloading) + " times meanwhile");
  }
}

void OtaUpdater::finish()
{
  Serial.println("OTA: download complete, " + String(totalBytes) + " bytes in " +
                 String((millis() - startedAt) / 1000UL) + " s, loop ran " + String(loopsWhileDownloading) +
                 " times while it was in flight");

  int code = ota.verify();
  if (code != OTAUpdate::OTA_ERROR_NONE)
  {
    fail("verify", code);
    return;
  }

  // verify() checks the CRC of what was received, not of what was written, and the bridge
  // ignores a failed fwrite, so a full filesystem truncates silently. The image is read back.
  if (!newImageMatchesManifest())
  {
    fail("read-back of the new image", 0);
    return;
  }
  // Checked again after the download and before the only step that cannot be taken back.
  if (!copyMatchesRunning())
  {
    state.copySaved = 0;
    save();
    fail("read-back of the copy on the SD card", 0);
    return;
  }
  install();
}

void OtaUpdater::install()
{
  downloading = false;
  if (strcmp(state.trial, target.c_str()) != 0)
  {
    state.installAttempts = 0;
  }
  state.phase = TRIAL;
  copyVersion(state.trial, target);
  state.trialBoots = 0;
  state.rollbackRequested = 0;
  save();

  Serial.println("OTA: image verified, installing " + target + " from " + IMAGE_PATH + " on trial; the copy of " +
                 String(state.good) + " is on the SD card; the board will restart");
  Serial.flush();
  delay(100);

  // On success the ESP32 reprograms the RA4M1 and restarts it, so this never returns.
  int code = ota.update(IMAGE_PATH);
  state.phase = IDLE;
  save();
  fail("update", code);
}

void OtaUpdater::fail(const String &step, int code, bool bridgeTouched)
{
  downloading = false;
  Serial.println("OTA: " + step + " failed (" + String(code) + ") on " + target + ", attempt " +
                 String(state.attempts) + " of " + String(MAX_DOWNLOAD_ATTEMPTS) + "; staying on " + FIRMWARE_VERSION);
  if (bridgeTouched)
  {
    restartBridge();
  }
}

// The bridge keeps the state of a download that failed, and the next download then crashes it
// a few seconds in: on this unit, every attempt after a failed one restarted the ESP32 and
// timed out. So the ESP32 is restarted here, deliberately, while nothing is being written. It
// costs a WiFi association, which the next request makes anyway; the RA4M1 carries on.
void OtaUpdater::restartBridge()
{
  std::string res;
  // +RESET calls ESP.restart() and so never answers; the modem gives up after its timeout.
  modem.write(std::string(PROMPT(_RESET)), res, "%s", CMD(_RESET));
  Watchdog::progress();
  Serial.println("OTA: restarted the ESP32 so the next attempt starts clean, bridge " + String(WiFi.firmwareVersion()));
}

// ---------------------------------------------------------------------------------------------
// The copy on the SD card

// Writes the running image, straight from code flash, to the card and reads it back.
bool OtaUpdater::saveCopy()
{
  copyTried = true;
  lastCopyAt = millis();
  const uint32_t size = runningImageSize();
  unsigned long t0 = millis();

  // setup() starts the card, but it may have been missing then. Starting it again is harmless.
  SD.begin(sdChipSelect);
  SD.remove(COPY_FILE);
  File f = SD.open(COPY_FILE, FILE_WRITE);
  if (!f)
  {
    Serial.println("OTA: could not create " + String(COPY_FILE) + " on the SD card; no updates until it can");
    return false;
  }
  uint32_t written = 0;
  while (written < size)
  {
    size_t n = min((uint32_t)512, size - written);
    if (f.write(__ROM_Start + written, n) != n)
    {
      break;
    }
    written += n;
    Watchdog::progress();
  }
  f.close();
  if (written != size)
  {
    Serial.println("OTA: writing the copy to the SD card stopped at " + String(written) + " of " + String(size) +
                   " bytes; no updates until it can be saved");
    return false;
  }

  uint32_t crc;
  if (!readCopy(size, crc, __ROM_Start))
  {
    return false;
  }
  state.copySaved = 1;
  state.copySize = size;
  state.copyCrc = crc;
  save();
  Serial.println("OTA: rollback copy of " + String(state.good) + " ready on the SD card, " + String(size) +
                 " bytes, crc " + String(crc, HEX) + ", read back identical to the running image (" +
                 String((millis() - t0) / 1000UL) + " s)");
  return true;
}

// Only while IDLE, when the running image is the good version.
bool OtaUpdater::copyMatchesRunning()
{
  uint32_t crc;
  if (!state.copySaved || !readCopy(runningImageSize(), crc, __ROM_Start))
  {
    return false;
  }
  Serial.println("OTA: the copy on the SD card read back identical to the running image");
  return true;
}

// The copy's length must be `expectedSize` and, when `compareTo` is given, every byte must
// match that memory. Returns the CRC-32 of what was read.
bool OtaUpdater::readCopy(uint32_t expectedSize, uint32_t &crc, const uint8_t *compareTo)
{
  crc = 0;
  File f = SD.open(COPY_FILE, FILE_READ);
  if (!f)
  {
    Serial.println("OTA: no " + String(COPY_FILE) + " on the SD card");
    return false;
  }
  if (f.size() != expectedSize)
  {
    Serial.println("OTA: " + String(COPY_FILE) + " on the SD card is " + String((uint32_t)f.size()) +
                   " bytes, expected " + String(expectedSize));
    f.close();
    return false;
  }
  uint8_t buf[256];
  uint32_t done = 0;
  while (done < expectedSize)
  {
    int n = f.read(buf, min((uint32_t)sizeof(buf), expectedSize - done));
    if (n <= 0)
    {
      break;
    }
    if (compareTo != nullptr && memcmp(buf, compareTo + done, n) != 0)
    {
      Serial.println("OTA: " + String(COPY_FILE) + " on the SD card differs from the running image near byte " +
                     String(done));
      f.close();
      return false;
    }
    crc = crc32Update(crc, buf, n);
    done += n;
    Watchdog::progress();
  }
  f.close();
  if (done != expectedSize)
  {
    Serial.println("OTA: reading " + String(COPY_FILE) + " from the SD card stopped at byte " + String(done));
    return false;
  }
  return true;
}

// Puts the copy of `good` on the ESP32, ready to flash, and reads it back from there. Runs on
// the image being rolled back, possibly before setup() has started the card.
bool OtaUpdater::restoreCopy()
{
  unsigned long t0 = millis();
  SD.begin(sdChipSelect);
  File f = SD.open(COPY_FILE, FILE_READ);
  if (!f)
  {
    Serial.println("OTA: no " + String(COPY_FILE) + " on the SD card");
    return false;
  }
  if (f.size() != state.copySize)
  {
    Serial.println("OTA: " + String(COPY_FILE) + " on the SD card is " + String((uint32_t)f.size()) +
                   " bytes, saved as " + String(state.copySize));
    f.close();
    return false;
  }
  if (!clearEsp32())
  {
    f.close();
    return false;
  }

  uint8_t buf[APPEND_CHUNK];
  uint32_t done = 0, crc = 0;
  while (done < state.copySize)
  {
    int n = f.read(buf, min((uint32_t)APPEND_CHUNK, state.copySize - done));
    if (n <= 0)
    {
      break;
    }
    // The bridge reads exactly `n` raw bytes after the command line, then answers OK.
    std::string res;
    modem.write_nowait(std::string(PROMPT(_FILESYSTEM)), res, "%s%d,%d,%s,%d\r\n", CMD_WRITE(_FILESYSTEM), 0,
                       WIFI_FILE_APPEND, IMAGE_PATH, n);
    if (!modem.passthrough(buf, n))
    {
      Serial.println("OTA: writing the copy to the ESP32 failed at byte " + String(done));
      f.close();
      return false;
    }
    crc = crc32Update(crc, buf, n);
    done += n;
    Watchdog::progress();
  }
  f.close();
  if (done != state.copySize || crc != state.copyCrc)
  {
    Serial.println("OTA: the copy on the SD card is damaged: " + String(done) + " bytes, crc " + String(crc, HEX) +
                   "; it was saved as " + String(state.copySize) + " bytes, crc " + String(state.copyCrc, HEX));
    return false;
  }

  // The bridge ignores a short write, so what landed is read back before it is flashed.
  uint32_t size, readCrc;
  if (!readImage(size, readCrc) || size != state.copySize || readCrc != state.copyCrc)
  {
    Serial.println("OTA: the copy read back from the ESP32 is " + String(size) + " bytes, crc " + String(readCrc, HEX) +
                   "; expected " + String(state.copySize) + " bytes, crc " + String(state.copyCrc, HEX));
    return false;
  }
  Serial.println("OTA: copy of " + String(state.good) + " written to the ESP32 and read back identical (" +
                 String((millis() - t0) / 1000UL) + " s)");
  return true;
}

// ---------------------------------------------------------------------------------------------
// The ESP32's filesystem

// Deletes every image path OTA code has used and leaves the filesystem mounted for the file
// commands. begin() mounts with format-on-fail, the one repair the bridge offers, and deletes
// IMAGE_PATH itself.
bool OtaUpdater::clearEsp32()
{
  int code = ota.begin(IMAGE_PATH);
  if (code != OTAUpdate::OTA_ERROR_NONE || !mountEsp32())
  {
    Serial.println("OTA: the ESP32 filesystem would not mount (" + String(code) + ")");
    return false;
  }
  for (const char *path : STALE_PATHS)
  {
    std::string res;
    modem.write(DO_NOT_CHECK_CMD, res, "%s%d,%d,%s\r\n", CMD_WRITE(_FILESYSTEM), 0, WIFI_FILE_DELETE, path);
    Watchdog::progress();
  }
  return true;
}

// Streams the image back over the AT link in 1 KB chunks: its size and CRC-32. About 15 s.
bool OtaUpdater::readImage(uint32_t &size, uint32_t &crc)
{
  std::string res;
  size = 0;
  crc = 0;
  if (!mountEsp32())
  {
    Serial.println("OTA: read-back of " + String(IMAGE_PATH) + ": the ESP32 filesystem would not mount");
    return false;
  }
  while (true)
  {
    res.clear();
    modem.avoid_trim_results();
    bool ok = modem.write(DO_NOT_CHECK_CMD, res, "%s%d,%d,%s,%lu,%d\r\n", CMD_WRITE(_FILESYSTEM), 0,
                          WIFI_FILE_READ, IMAGE_PATH, (unsigned long)size, READ_CHUNK);
    Watchdog::progress();
    if (!ok)
    {
      if (size == 0)
      {
        Serial.println("OTA: read-back of " + String(IMAGE_PATH) + ": no such file");
        return false;
      }
      break; // a read exactly at the end of a file whose size is a multiple of the chunk
    }
    crc = crc32Update(crc, (const uint8_t *)res.data(), res.size());
    size += res.size();
    if ((int)res.size() < READ_CHUNK)
    {
      break;
    }
  }
  return true;
}

// A new image has nothing on the board to compare with, so the manifest describes it.
bool OtaUpdater::newImageMatchesManifest()
{
  uint32_t size, crc;
  if (!readImage(size, crc))
  {
    return false;
  }
  if (size != targetSize || crc != targetCrc)
  {
    Serial.println("OTA: read-back of " + String(IMAGE_PATH) + ": " + String(size) + " bytes, crc " + String(crc, HEX) +
                   "; the manifest says " + String(targetSize) + " bytes, crc " + String(targetCrc, HEX));
    return false;
  }
  Serial.println("OTA: read-back of " + String(IMAGE_PATH) + ": " + String(size) + " bytes, crc " + String(crc, HEX) +
                 ", as the manifest says");
  return true;
}

// ---------------------------------------------------------------------------------------------
// State

void OtaUpdater::load()
{
  State r[2];
  EEPROM.get(RECORD_ADDRESS[0], r[0]);
  EEPROM.get(RECORD_ADDRESS[1], r[1]);
  bool v0 = recordValid(r[0]);
  bool v1 = recordValid(r[1]);

  if (!v0 && !v1)
  {
    Serial.println("OTA: no saved state, starting fresh");
    memset(&state, 0, sizeof(state));
    state.magic = STATE_MAGIC;
    state.phase = IDLE;
    memset(&saved, 0, sizeof(saved));
    record = 1; // so the first save goes to record 0
    return;
  }
  record = (v0 && (!v1 || r[0].generation >= r[1].generation)) ? 0 : 1;
  state = r[record];
  saved = state;
  state.good[VERSION_LEN - 1] = state.trial[VERSION_LEN - 1] = state.rejected[VERSION_LEN - 1] =
      state.attempted[VERSION_LEN - 1] = '\0';
}

void OtaUpdater::save()
{
  // Data flash wears out: only write what changed. Compared without the bookkeeping fields.
  State a = state, b = saved;
  a.generation = b.generation = 0;
  a.checksum = b.checksum = 0;
  if (memcmp(&a, &b, sizeof(State)) == 0)
  {
    return;
  }
  // Always the other record, so the last one written stays intact until this one is.
  state.generation = saved.generation + 1;
  state.checksum = checksumOf(state);
  record = record ? 0 : 1;
  EEPROM.put(RECORD_ADDRESS[record], state);
  saved = state;
}

bool OtaUpdater::recordValid(const State &s)
{
  return s.magic == STATE_MAGIC && s.checksum == checksumOf(s) && s.phase <= TRIAL && s.copySaved <= 1;
}

uint32_t OtaUpdater::checksumOf(const State &s)
{
  // FNV-1a over everything before the checksum field.
  const uint8_t *p = (const uint8_t *)&s;
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < offsetof(State, checksum); i++)
  {
    h = (h ^ p[i]) * 16777619UL;
  }
  return h;
}

void OtaUpdater::copyVersion(char *dst, const String &src)
{
  strncpy(dst, src.c_str(), VERSION_LEN - 1);
  dst[VERSION_LEN - 1] = '\0';
}

// A version is trusted only if it looks like one. An error page, an empty body or a stray
// newline is not a version, and must not become a download URL.
bool OtaUpdater::validVersion(const String &version)
{
  if (version.length() == 0 || version.length() >= VERSION_LEN)
  {
    return false;
  }
  for (unsigned int i = 0; i < version.length(); i++)
  {
    char c = version[i];
    if (!isAlphaNumeric(c) && c != '.' && c != '_' && c != '-')
    {
      return false;
    }
  }
  return true;
}

// "<version> <bytes> <crc32 hex>". Size and CRC describe the decompressed image, which is
// what lands on the ESP32 and what the RA4M1 ends up running.
bool OtaUpdater::parseManifest(const String &body, String &version, uint32_t &size, uint32_t &crc)
{
  String s = body;
  s.trim();
  int a = s.indexOf(' ');
  int b = a < 0 ? -1 : s.indexOf(' ', a + 1);
  if (a < 0 || b < 0)
  {
    return false;
  }
  version = s.substring(0, a);
  String sizeText = s.substring(a + 1, b);
  String crcText = s.substring(b + 1);
  if (!validVersion(version) || sizeText.length() == 0 || crcText.length() == 0 || crcText.length() > 8)
  {
    return false;
  }
  char *end;
  size = strtoul(sizeText.c_str(), &end, 10);
  if (*end != '\0')
  {
    return false;
  }
  crc = strtoul(crcText.c_str(), &end, 16);
  return *end == '\0';
}

String OtaUpdater::describe() const
{
  String s = String("running ") + FIRMWARE_VERSION + ", phase " + (state.phase == TRIAL ? "TRIAL" : "IDLE") +
             ", good " + (state.good[0] ? state.good : "(none)") + " copy " +
             (state.copySaved ? String("on the SD card, ") + String(state.copySize) + " bytes" : String("missing"));
  if (state.phase == TRIAL)
  {
    s += ", trial " + String(state.trial) + " boots " + String(state.trialBoots);
  }
  if (state.rejected[0])
  {
    s += ", rejected " + String(state.rejected);
  }
  s += ", record " + String(record) + " gen " + String(state.generation);
  return s;
}
