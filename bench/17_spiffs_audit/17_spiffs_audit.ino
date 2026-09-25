// 17_spiffs_audit — what is in the ESP32's SPIFFS, read-only.
//
// The bridge has no list or usage command, so every path OTA code has ever used is read in
// full over the AT link: whether it exists, its size, its CRC-32. /b.bin is read three times
// to show whether the read path itself is repeatable. Nothing is written or deleted. The
// results are replayed every 10 s so a late capture still sees them.
#include <WiFiS3.h>
#include "Modem.h"
#include "WiFiCommands.h"

static String results[24];
static int count = 0;
static void note(const String &s) { Serial.println(s); if (count < 24) results[count++] = s; }

static uint32_t crc32Update(uint32_t crc, const uint8_t *d, size_t n)
{
  crc = ~crc;
  while (n--) { crc ^= *d++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL))); }
  return ~crc;
}

static void audit(const char *path)
{
  std::string res;
  uint32_t size = 0, crc = 0;
  unsigned long t0 = millis();
  while (true)
  {
    res.clear();
    modem.avoid_trim_results();
    bool ok = modem.write(DO_NOT_CHECK_CMD, res, "%s%d,%d,%s,%lu,%d\r\n", CMD_WRITE(_FILESYSTEM), 0, WIFI_FILE_READ, path,
                          (unsigned long)size, 1023);
    if (!ok) break;
    crc = crc32Update(crc, (const uint8_t *)res.data(), res.size());
    size += res.size();
    if (res.size() < 1023) break;
  }
  if (size == 0) note(String("AUDIT ") + path + " absent");
  else note(String("AUDIT ") + path + " " + String(size) + " bytes crc " + String(crc, HEX) + " (" + String((millis() - t0) / 1000) + " s)");
}

void setup()
{
  Serial.begin(9600);
  delay(5000);
  note("AUDIT start, bridge " + String(WiFi.firmwareVersion()));
  std::string res;
  note(String("AUDIT mount ") + (modem.write(std::string(PROMPT(_MOUNTFS)), res, "%s%d\r\n", CMD_WRITE(_MOUNTFS), 0) ? "ok" : "FAILED"));
  audit("/update.bin");
  audit("/a.bin");
  audit("/b.bin");
  audit("/b.bin");
  audit("/b.bin");
  note("AUDIT done");
}

void loop()
{
  static unsigned long last = 0;
  if (millis() - last >= 10000) { last = millis(); for (int i = 0; i < count; i++) Serial.println("  " + results[i]); }
}
