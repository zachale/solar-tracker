// 19_spiffs_reclaim — does the ESP32's SPIFFS get its space back once every image is deleted?
//
// 18_slot_stress found that a second 151 KB image no longer fits beside the first, in a
// 512 KB filesystem. This deletes every path OTA code has used, then downloads a known image
// (s3) into /a.bin four times, then into /b.bin four times with /a.bin kept. Each download is
// read back in full and checked against the size and CRC-32 computed on the Mac, and /a.bin is
// read again after every /b.bin round to show it survived the garbage collection.
//   - /b.bin rounds pass: the space was held by deleted or orphaned pages, and deleting the
//     slots is enough to get it back. The firmware can do that by itself.
//   - /a.bin passes, /b.bin keeps failing: the space is held by something no delete of a
//     known path reaches, and only a format gets it back.
// Deletes nothing but /a.bin, /b.bin and /update.bin. Never flashes anything.
#include <WiFiS3.h>
#include <OTAUpdate.h>
#include "secrets.h"
#define BENCH_WIFI
#include "bench_common.h"
namespace
{
#include "root_ca.h"
}

static const uint32_t EXPECT_SIZE = 151512UL;
static const uint32_t EXPECT_CRC = 0x7f57b7ffUL;
OTAUpdate ota;

static uint32_t crc32Update(uint32_t crc, const uint8_t *d, size_t n)
{
  crc = ~crc;
  while (n--) { crc ^= *d++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL))); }
  return ~crc;
}

static bool mountFs()
{
  std::string res;
  return modem.write(std::string(PROMPT(_MOUNTFS)), res, "%s%d\r\n", CMD_WRITE(_MOUNTFS), 0);
}

static bool removeFile(const char *path)
{
  std::string res;
  // The bridge answers a delete with a bare OK, no +FS prompt to match.
  return modem.write(DO_NOT_CHECK_CMD, res, "%s%d,%d,%s\r\n", CMD_WRITE(_FILESYSTEM), 0, WIFI_FILE_DELETE, path);
}

static void readBack(const char *path, uint32_t &size, uint32_t &crc)
{
  std::string res;
  size = crc = 0;
  mountFs();
  while (true)
  {
    res.clear();
    modem.avoid_trim_results();
    if (!modem.write(DO_NOT_CHECK_CMD, res, "%s%d,%d,%s,%lu,%d\r\n", CMD_WRITE(_FILESYSTEM), 0, WIFI_FILE_READ, path,
                     (unsigned long)size, 1023)) break;
    crc = crc32Update(crc, (const uint8_t *)res.data(), res.size());
    size += res.size();
    if (res.size() < 1023) break;
  }
}

static bool intact(const char *path, String &detail)
{
  uint32_t size, crc;
  readBack(path, size, crc);
  detail = String(size) + " bytes crc " + String(crc, HEX);
  return size == EXPECT_SIZE && crc == EXPECT_CRC;
}

static bool download(const char *path, unsigned long &seconds)
{
  String url = String(OTA_BASE_URL) + "/s3.ota";
  unsigned long t0 = millis();
  ota.begin(path);
  ota.setCACert(root_ca);
  int total = ota.startDownload(url.c_str(), path);
  int done = 0;
  while (total > 0 && millis() - t0 < 240000UL)
  {
    done = ota.downloadProgress();
    if (done < 0 || done >= total) break;
    delay(250);
  }
  seconds = (millis() - t0) / 1000UL;
  return total > 0 && done >= total && ota.verify() == 0;
}

void setup()
{
  Serial.begin(9600);
  delay(5000);
  SD.begin(PIN_SD_CS);
  Serial.println(String("RECLAIM wifi ") + (benchConnectAny() ? "ok" : "FAILED"));
  Serial.println(String("RECLAIM mount ") + (mountFs() ? "ok" : "FAILED"));
  const char *paths[] = {"/a.bin", "/b.bin", "/update.bin"};
  for (const char *p : paths) Serial.println(String("RECLAIM delete ") + p + (removeFile(p) ? " ok" : " (absent or failed)"));

  int passA = 0, passB = 0;
  for (int round = 1; round <= 4; round++)
  {
    unsigned long s;
    bool dl = download("/a.bin", s);
    String d;
    bool ok = dl && intact("/a.bin", d);
    if (ok) passA++;
    Serial.println("RECLAIM a round " + String(round) + " " + (ok ? "PASS" : "FAIL") + " download " + String(s) + " s" +
                   (dl ? "" : " (transfer failed)") + ", read " + d);
    delay(2000);
  }
  // /b.bin only means something beside an intact /a.bin, so make sure one is there.
  String d;
  if (!intact("/a.bin", d))
  {
    unsigned long s;
    download("/a.bin", s);
  }
  for (int round = 1; round <= 4; round++)
  {
    unsigned long s;
    bool dl = download("/b.bin", s);
    String db, da;
    bool okB = dl && intact("/b.bin", db);
    bool okA = intact("/a.bin", da);
    if (okB && okA) passB++;
    Serial.println("RECLAIM b round " + String(round) + " " + (okB && okA ? "PASS" : "FAIL") + " download " + String(s) + " s" +
                   (dl ? "" : " (transfer failed)") + ", /b.bin " + db + ", /a.bin " + da);
    delay(2000);
  }
  Serial.println("RECLAIM done: a " + String(passA) + " of 4, b " + String(passB) + " of 4");
}

void loop() {}
