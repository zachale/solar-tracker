#include "OtaUpdater.h"
#include <EEPROM.h>
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

const uint32_t STATE_MAGIC = 0x4F544133; // "OTA3"
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
const unsigned long SOON_MS = 10000UL;              // follow-up check after a seed or confirm
const unsigned long ROLLBACK_RETRY_MS = 60000UL;
const uint8_t MAX_TRIAL_BOOTS = 3;
const uint8_t MAX_INSTALL_ATTEMPTS = 3;
const int MAX_DOWNLOAD_ATTEMPTS = 3;

const unsigned long POLL_INTERVAL_MS = 250UL; // each poll is one AT round trip
const unsigned long LOG_INTERVAL_MS = 2000UL;
const unsigned long STALL_TIMEOUT_MS = 60000UL;
const int READ_CHUNK = 1023;

// The running sketch in code flash, from the linker script. It starts at __ROM_Start (0x4000
// on this board; not SCB->VTOR, which main.cpp points at a copy of the vector table in RAM)
// and the .bin every flash writes is the code up to __etext followed by the initial values of
// .data. tools/ota/build.sh checks this formula against every image it builds.
extern "C" const uint8_t __ROM_Start[], __etext[], __data_start__[], __data_end__[];
const uint32_t CODE_FLASH_END = 0x40000UL; // 256 KB

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
} // namespace

// ---------------------------------------------------------------------------------------------
// Boot

void OtaUpdater::boot()
{
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
        Serial.println("OTA: ROLLED BACK from " + String(state.trial) + " to " + running + "; " +
                       String(state.trial) + " is rejected and will not be installed again");
        copyVersion(state.rejected, state.trial);
        state.trial[0] = '\0';
        // Flashing the rollback copy consumed it.
        state.goodSlotValid = 0;
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
    // A new base, usually a USB flash. Its rollback copy has to be downloaded before any
    // trial can start, because the slot holds some other version.
    copyVersion(state.good, running);
    state.goodSlotValid = 0;
    if (running == state.rejected)
    {
      state.rejected[0] = '\0';
    }
    Serial.println("OTA: " + running + " is the new base; its rollback copy will be downloaded first");
  }
  save();
}

// ---------------------------------------------------------------------------------------------
// Loop

bool OtaUpdater::busy() const
{
  return job != NO_JOB;
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

  if (job != NO_JOB)
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

  if (!state.goodSlotValid)
  {
    // No trial may start without a verified copy of what is running now.
    startDownload(SEED, running, otherSlot(state.goodSlot));
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
  if (wanted == failedVersion && failedAttempts >= MAX_DOWNLOAD_ATTEMPTS)
  {
    Serial.println("OTA: " + wanted + " failed " + String(failedAttempts) +
                   " times; not trying again until the manifest changes or the board restarts");
    return;
  }
  if (!Watchdog::armed())
  {
    // Without a watchdog a new image that hangs is never reset, so it never rolls back.
    Serial.println("OTA: not updating to " + wanted + ": this image has no working watchdog");
    return;
  }
  // The net is checked before every jump, not trusted from a flag: the copy is read back and
  // compared with the running image in full.
  if (!copyMatchesRunning(state.goodSlot))
  {
    Serial.println("OTA: the rollback copy in " + String(slotPath(state.goodSlot)) +
                   " no longer matches the running image; downloading it again before any update");
    state.goodSlotValid = 0;
    save();
    startDownload(SEED, running, otherSlot(state.goodSlot));
    return;
  }

  Serial.println("OTA: manifest wants " + wanted + " (" + String(wantedSize) + " bytes), running " + running);
  targetSize = wantedSize;
  targetCrc = wantedCrc;
  startDownload(INSTALL, wanted, otherSlot(state.goodSlot));
}

void OtaUpdater::confirm()
{
  const String running = FIRMWARE_VERSION;
  Serial.println("OTA: " + running + " CONFIRMED after " + String(millis() / 1000UL) + " s and " +
                 String(state.trialBoots) + " boot(s); it is now the good version");
  copyVersion(state.good, running);
  // The slot still holds the previous good version, not this one, so a fresh copy is
  // downloaded before anything else is tried.
  state.goodSlotValid = 0;
  state.phase = IDLE;
  state.trial[0] = '\0';
  state.trialBoots = 0;
  state.rollbackRequested = 0;
  state.installAttempts = 0;
  save();
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

  if (!state.goodSlotValid)
  {
    // Cannot happen through this code: a trial starts only after the copy has been read back
    // intact, and installing the trial consumes only the trial's own slot. If it ever does,
    // this image stays on trial; if it is sound it can still confirm.
    Serial.println("OTA: CANNOT ROLL BACK (" + reason + "): no copy of " + String(state.good) + "; staying on trial");
    return;
  }

  state.rollbackRequested = 1;
  save();
  Serial.println("OTA: ROLLING BACK to " + String(state.good) + " from " + slotPath(state.goodSlot) + ": " + reason);
  Serial.flush();
  delay(100);

  // On success the ESP32 flashes the saved copy and restarts the RA4M1: no return.
  int code = ota.update(slotPath(state.goodSlot));

  // Still here, so nothing was flashed: the ESP32 did not answer, or could not open the copy.
  // Nothing else changes, and the next boot or the next minute tries again.
  Serial.println("OTA: the rollback did not happen (" + String(code) + "); trying again in " +
                 String(ROLLBACK_RETRY_MS / 1000UL) + " s");
  state.rollbackRequested = 0;
  save();
}

// ---------------------------------------------------------------------------------------------
// Downloads

void OtaUpdater::startDownload(Job what, const String &version, uint8_t slot)
{
  const char *path = slotPath(slot);
  String url = String(OTA_BASE_URL) + "/" + version + ".ota";

  // WiFiS3 formats every AT command into a MAX_BUFF_SIZE byte buffer (128, Modem.h) with
  // vsnprintf and sends whatever fits. A longer command loses its closing newline: the
  // bridge never runs it, the RA4M1 waits out the 60 s timeout for an answer that is not
  // coming, and the unfinished line corrupts the next command too. That is what a 129 byte
  // startDownload did on this unit; see tools/ota/README.md.
  size_t line = strlen(CMD_WRITE(_OTA_DOWNLOAD_START)) + url.length() + 1 + strlen(path) + strlen(_ENDL);
  target = version;
  targetSlot = slot;
  job = what;
  if (line >= MAX_BUFF_SIZE)
  {
    fail("URL too long: startDownload would be " + String(line) + " bytes, the modem sends at most " +
             String(MAX_BUFF_SIZE - 1),
         0);
    return;
  }

  Serial.println(what == SEED ? "OTA: downloading " + version + " as its own rollback copy into " + path
                              : "OTA: downloading " + version + " into " + path);

  // begin() deletes this slot's file, which is why nothing ever downloads into the slot
  // holding the rollback copy.
  int code = ota.begin(path);
  if (code != OTAUpdate::OTA_ERROR_NONE)
  {
    fail("begin", code);
    return;
  }
  code = ota.setCACert(root_ca);
  if (code != OTAUpdate::OTA_ERROR_NONE)
  {
    fail("setCACert", code);
    return;
  }

  // startDownload() waits for the connection and the response headers, which is how it
  // learns the size; that part takes a second or two. The transfer itself runs on the ESP32
  // from here on, and the tracker carries on.
  unsigned long t0 = millis();
  int size = ota.startDownload(url.c_str(), path);
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
  // ignores a failed fwrite, so a full filesystem truncates silently. Every file is read back.
  if (job == SEED)
  {
    job = NO_JOB;
    if (!copyMatchesRunning(targetSlot))
    {
      fail("read-back of the rollback copy", 0);
      return;
    }
    state.goodSlot = targetSlot;
    state.goodSlotValid = 1;
    save();
    Serial.println("OTA: rollback copy of " + target + " ready in " + slotPath(targetSlot) +
                   ", read back identical to the running image");
    checkSoon = true;
    return;
  }

  if (!newImageMatchesManifest(targetSlot))
  {
    fail("read-back of the new image", 0);
    return;
  }
  install();
}

void OtaUpdater::install()
{
  job = NO_JOB;
  if (strcmp(state.trial, target.c_str()) != 0)
  {
    state.installAttempts = 0;
  }
  state.phase = TRIAL;
  copyVersion(state.trial, target);
  state.trialSlot = targetSlot;
  state.trialBoots = 0;
  state.rollbackRequested = 0;
  save();

  Serial.println("OTA: image verified, installing " + target + " from " + slotPath(targetSlot) +
                 " on trial; rollback copy of " + String(state.good) + " is in " + slotPath(state.goodSlot) +
                 "; the board will restart");
  Serial.flush();
  delay(100);

  // On success the ESP32 reprograms the RA4M1 and restarts it, so this never returns.
  int code = ota.update(slotPath(targetSlot));
  state.phase = IDLE;
  save();
  fail("update", code);
}

void OtaUpdater::fail(const String &step, int code)
{
  job = NO_JOB;
  if (target == failedVersion)
  {
    failedAttempts++;
  }
  else
  {
    failedVersion = target;
    failedAttempts = 1;
  }
  Serial.println("OTA: " + step + " failed (" + String(code) + ") on " + target + ", attempt " +
                 String(failedAttempts) + " of " + String(MAX_DOWNLOAD_ATTEMPTS) + "; staying on " + FIRMWARE_VERSION);
}

// ---------------------------------------------------------------------------------------------
// Reading files back from the ESP32

// Streams a slot back over the AT link in 1 KB chunks: its size, its CRC-32, and, when
// `compareTo` is given, whether every byte matches that memory. About 15 s for an image.
bool OtaUpdater::readSlot(uint8_t slot, uint32_t &size, uint32_t &crc, const uint8_t *compareTo)
{
  const char *path = slotPath(slot);
  const uint32_t limit = CODE_FLASH_END - (uint32_t)__ROM_Start;
  std::string res;
  size = 0;
  crc = 0;

  // Mount without ever formatting: 0 is format-on-fault off. The download and flash calls
  // mount and unmount on their own; the read command assumes it is already mounted.
  if (!modem.write(std::string(PROMPT(_MOUNTFS)), res, "%s%d\r\n", CMD_WRITE(_MOUNTFS), 0))
  {
    Serial.println("OTA: read-back of " + String(path) + ": the ESP32 filesystem would not mount");
    return false;
  }

  while (true)
  {
    res.clear();
    modem.avoid_trim_results();
    bool ok = modem.write(DO_NOT_CHECK_CMD, res, "%s%d,%d,%s,%lu,%d\r\n", CMD_WRITE(_FILESYSTEM), 0,
                          WIFI_FILE_READ, path, (unsigned long)size, READ_CHUNK);
    Watchdog::progress();
    if (!ok)
    {
      if (size == 0)
      {
        Serial.println("OTA: read-back of " + String(path) + ": no such file");
        return false;
      }
      break; // a read exactly at the end of a file whose size is a multiple of the chunk
    }
    const uint8_t *data = (const uint8_t *)res.data();
    if (compareTo != nullptr && (size + res.size() > limit || memcmp(data, compareTo + size, res.size()) != 0))
    {
      Serial.println("OTA: read-back of " + String(path) + ": differs from the running image near byte " +
                     String(size));
      return false;
    }
    crc = crc32Update(crc, data, res.size());
    size += res.size();
    if ((int)res.size() < READ_CHUNK)
    {
      break;
    }
  }
  return true;
}

// The rollback copy must be exactly the running image: the same length, and identical byte
// for byte. Only used while IDLE, when the running image is the good version.
bool OtaUpdater::copyMatchesRunning(uint8_t slot)
{
  const uint32_t expected = runningImageSize();
  uint32_t size, crc;
  unsigned long t0 = millis();
  if (!readSlot(slot, size, crc, __ROM_Start))
  {
    return false;
  }
  if (size != expected)
  {
    Serial.println("OTA: read-back of " + String(slotPath(slot)) + ": " + String(size) + " bytes, the running image is " +
                   String(expected));
    return false;
  }
  Serial.println("OTA: read-back of " + String(slotPath(slot)) + ": " + String(size) +
                 " bytes identical to the running image (" + String((millis() - t0) / 1000UL) + " s)");
  return true;
}

// A new image has nothing on the board to compare with, so the manifest describes it.
bool OtaUpdater::newImageMatchesManifest(uint8_t slot)
{
  uint32_t size, crc;
  if (!readSlot(slot, size, crc, nullptr))
  {
    return false;
  }
  if (size != targetSize || crc != targetCrc)
  {
    Serial.println("OTA: read-back of " + String(slotPath(slot)) + ": " + String(size) + " bytes, crc " +
                   String(crc, HEX) + "; the manifest says " + String(targetSize) + " bytes, crc " +
                   String(targetCrc, HEX));
    return false;
  }
  Serial.println("OTA: read-back of " + String(slotPath(slot)) + ": " + String(size) + " bytes, crc " +
                 String(crc, HEX) + ", as the manifest says");
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
  state.good[VERSION_LEN - 1] = state.trial[VERSION_LEN - 1] = state.rejected[VERSION_LEN - 1] = '\0';
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
  record = otherSlot(record);
  EEPROM.put(RECORD_ADDRESS[record], state);
  saved = state;
}

bool OtaUpdater::recordValid(const State &s)
{
  return s.magic == STATE_MAGIC && s.checksum == checksumOf(s) && s.phase <= TRIAL && s.goodSlot <= 1 &&
         s.trialSlot <= 1;
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

const char *OtaUpdater::slotPath(uint8_t slot)
{
  return slot ? "/b.bin" : "/a.bin";
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
// what lands in the slot and what the RA4M1 ends up running.
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
             (state.goodSlotValid ? String("in ") + slotPath(state.goodSlot) : String("missing"));
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
