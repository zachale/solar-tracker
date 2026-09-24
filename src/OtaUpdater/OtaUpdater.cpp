#include "OtaUpdater.h"
#include "../../secrets.h"

namespace
{
#include "root_ca.h"

// OTA_BASE_URL lives in the gitignored secrets.h, e.g.
//   #define OTA_BASE_URL "https://storage.googleapis.com/<bucket>/<token>"
// The token in that path is the only thing keeping the images unlisted, and this repo
// is public, so it must never be printed: serial captures end up committed.
const char *const SPIFFS_PATH = "/update.bin";

const unsigned long FIRST_CHECK_DELAY_MS = 30000UL; // after boot, once WiFi has settled
const unsigned long CHECK_INTERVAL_MS = 300000UL;   // five minutes
const unsigned long POLL_INTERVAL_MS = 250UL;       // each poll is one AT round trip
const unsigned long LOG_INTERVAL_MS = 2000UL;
const unsigned long STALL_TIMEOUT_MS = 60000UL;
const int MAX_ATTEMPTS_PER_VERSION = 3;
} // namespace

bool OtaUpdater::busy() const
{
  return state == DOWNLOADING;
}

void OtaUpdater::service(WifiClient &wifi, bool radioFree)
{
  if (state == DOWNLOADING)
  {
    loopsWhileDownloading++;
    // The ESP32 keeps downloading whether or not we ask how it is going, so skipping
    // polls while the actuator moves costs nothing but a later progress line.
    if (radioFree)
    {
      poll();
    }
    return;
  }

  if (!radioFree || !checkDue())
  {
    return;
  }
  firstCheckDone = true;
  lastCheckAt = millis();
  check(wifi);
}

bool OtaUpdater::checkDue() const
{
  if (!firstCheckDone)
  {
    return millis() >= FIRST_CHECK_DELAY_MS;
  }
  return millis() - lastCheckAt >= CHECK_INTERVAL_MS;
}

void OtaUpdater::check(WifiClient &wifi)
{
  if (String(FIRMWARE_VERSION) == "dev")
  {
    Serial.println("OTA: dev build, not checking for updates");
    return;
  }

  String wanted = wifi.get(String(OTA_BASE_URL) + "/manifest.txt");
  wanted.trim();
  if (!validVersion(wanted))
  {
    Serial.println("OTA: no usable manifest, staying on " FIRMWARE_VERSION);
    return;
  }
  if (wanted == FIRMWARE_VERSION)
  {
    Serial.println("OTA: up to date at " FIRMWARE_VERSION);
    return;
  }
  if (wanted == failedVersion && failedAttempts >= MAX_ATTEMPTS_PER_VERSION)
  {
    Serial.println("OTA: " + wanted + " failed " + String(failedAttempts) +
                   " times, not trying it again until the manifest changes or the board restarts");
    return;
  }
  start(wanted);
}

void OtaUpdater::start(const String &version)
{
  target = version;
  Serial.println("OTA: manifest wants " + version + ", running " FIRMWARE_VERSION);

  int code = ota.begin(SPIFFS_PATH);
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
  // learns the size; that part can take a few seconds. The transfer itself runs on the
  // ESP32 from here on, and the tracker carries on.
  String url = String(OTA_BASE_URL) + "/" + version + ".ota";
  unsigned long t0 = millis();
  int size = ota.startDownload(url.c_str(), SPIFFS_PATH);
  if (size <= 0)
  {
    fail("startDownload", size);
    return;
  }

  totalBytes = size;
  lastBytes = 0;
  loopsWhileDownloading = 0;
  startedAt = lastPollAt = lastLogAt = lastAdvanceAt = millis();
  state = DOWNLOADING;
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

  // Arduino's own example loops `while (downloadProgress() < size)`, which spins forever
  // on an error because every error code is negative. Errors are checked first here.
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

  Serial.println("OTA: image verified, flashing " + target + " now; the board will restart");
  Serial.flush();
  delay(100);

  // On success the ESP32 reprograms the RA4M1 and resets it, so this never returns.
  code = ota.update(SPIFFS_PATH);
  fail("update", code);
}

void OtaUpdater::fail(const String &step, int code)
{
  state = IDLE;
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
                 String(failedAttempts) + " of " + String(MAX_ATTEMPTS_PER_VERSION) + "; staying on " FIRMWARE_VERSION);
}

// A manifest is trusted only if it looks like a version name. An error page, an empty
// body or a stray newline is not a version, and must not become a download URL.
bool OtaUpdater::validVersion(const String &version)
{
  if (version.length() == 0 || version.length() > 32)
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
