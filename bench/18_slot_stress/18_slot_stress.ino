// 18_slot_stress — can two full-size images live on the ESP32 at once, reliably?
//
// /b.bin holds a 151 KB image throughout. This downloads a known 151 KB image (s3) into
// /a.bin ten times, reading each back in full and checking it against the size and CRC-32
// computed on the Mac. Each round prints its download time next to PASS or FAIL, which
// separates a filesystem too full to take the second image (fails at any speed) from a
// decoder that mishandles a slow network (fails only when slow). Never flashes anything.
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
static const int ROUNDS = 10;
OTAUpdate ota;

static uint32_t crc32Update(uint32_t crc, const uint8_t *d, size_t n)
{
  crc = ~crc;
  while (n--) { crc ^= *d++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL))); }
  return ~crc;
}

static bool readBack(const char *path, uint32_t &size, uint32_t &crc)
{
  std::string res;
  size = crc = 0;
  if (!modem.write(std::string(PROMPT(_MOUNTFS)), res, "%s%d\r\n", CMD_WRITE(_MOUNTFS), 0)) return false;
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
  return true;
}

void setup()
{
  Serial.begin(9600);
  delay(5000);
  SD.begin(PIN_SD_CS);
  Serial.println(String("STRESS wifi ") + (benchConnectAny() ? "ok" : "FAILED"));
  String url = String(OTA_BASE_URL) + "/s3.ota";
  int pass = 0;
  for (int round = 1; round <= ROUNDS; round++)
  {
    unsigned long t0 = millis();
    ota.begin("/a.bin");
    ota.setCACert(root_ca);
    int total = ota.startDownload(url.c_str(), "/a.bin");
    int done = 0;
    while (total > 0 && millis() - t0 < 180000UL)
    {
      done = ota.downloadProgress();
      if (done < 0 || done >= total) break;
      delay(250);
    }
    unsigned long dl = (millis() - t0) / 1000UL;
    int v = (total > 0 && done >= total) ? ota.verify() : -999;
    uint32_t size, crc;
    bool read = readBack("/a.bin", size, crc);
    bool ok = total > 0 && v == 0 && read && size == EXPECT_SIZE && crc == EXPECT_CRC;
    if (ok) pass++;
    Serial.println("STRESS round " + String(round) + " " + (ok ? "PASS" : "FAIL") + " download " + String(dl) + " s, verify " +
                   String(v) + ", read " + String(size) + " bytes crc " + String(crc, HEX));
    delay(3000);
  }
  Serial.println("STRESS done: " + String(pass) + " of " + String(ROUNDS) + " rounds intact");
}

void loop() {}
