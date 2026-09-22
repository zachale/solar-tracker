// 00 — read-only inventory of the SD card. WRITES NOTHING.
//
// Run this before anything else. It answers two questions: what is on the card,
// and is there spooled telemetry still waiting to reach the cloud. Telemetry
// chunks are Telemetry::pathFor(i) == "tlm<i>.csv", i in 0..63.

#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include "bench_common.h"

RTC_DS3231 rtc;
static const int MAX_CHUNKS = 64;

static unsigned long countRows(const char *path, uint32_t *firstTs, uint32_t *lastTs)
{
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  unsigned long rows = 0;
  String line;
  *firstTs = 0; *lastTs = 0;
  unsigned long guard = 0;
  while (f.available() && guard++ < BENCH_MAX_FILE_BYTES)
  {
    char c = f.read();
    if (c == '\n')
    {
      if (line.length() > 0)
      {
        rows++;
        int comma = line.indexOf(',');
        uint32_t ts = (comma > 0) ? (uint32_t)line.substring(0, comma).toInt() : 0;
        if (rows == 1) *firstTs = ts;
        *lastTs = ts;
      }
      line = "";
    }
    else if (c != '\r' && line.length() < 64) line += c;
  }
  if (line.length() > 0) rows++;
  f.close();
  return rows;
}

static void report()
{
  benchBanner("00_card_inventory");
  parkActuator();
  rtcRailOn();
  delay(200);
  Wire.begin();
  rtc.begin();
  RtcSnap s = rtcSnapshot(rtc);
  logSnap("00", 0, s, "EVENT=RTC_AT_BOOT");

  if (!SD.begin(PIN_SD_CS))
  {
    Serial.println("BENCH RESULT=FAIL reason=SD.begin failed");
    Serial.println("BENCH   this alone would explain zero telemetry uploads");
    return;
  }
  Serial.println("BENCH SD.begin ok");

  Serial.println("BENCH --- root listing ---");
  File root = SD.open("/");
  unsigned long entries = 0;
  while (root && entries++ < BENCH_MAX_DIR_ENTRIES)
  {
    File e = root.openNextFile();
    if (!e) break;
    Serial.print("BENCH   ");
    Serial.print(e.name());
    Serial.print("  ");
    Serial.print(e.isDirectory() ? 0 : e.size());
    Serial.println(e.isDirectory() ? " <dir>" : " bytes");
    e.close();
  }
  if (root) root.close();

  Serial.println("BENCH --- pending telemetry ---");
  unsigned long totalRows = 0, totalBytes = 0;
  int occupied = 0;
  for (int i = 0; i < MAX_CHUNKS; i++)
  {
    String p = "tlm" + String(i) + ".csv";
    File f = SD.open(p.c_str(), FILE_READ);
    if (!f) continue;
    size_t sz = f.size();
    f.close();
    if (sz == 0) continue;
    occupied++;
    uint32_t a = 0, b = 0;
    unsigned long rows = countRows(p.c_str(), &a, &b);
    totalRows += rows;
    totalBytes += sz;
    Serial.print("BENCH   ");
    Serial.print(p);
    Serial.print("  bytes=");
    Serial.print(sz);
    Serial.print(" rows=");
    Serial.print(rows);
    Serial.print(" first_unix=");
    Serial.print(a);
    Serial.print(" last_unix=");
    Serial.println(b);
  }
  Serial.print("BENCH PENDING chunks=");
  Serial.print(occupied);
  Serial.print(" rows=");
  Serial.print(totalRows);
  Serial.print(" bytes=");
  Serial.println(totalBytes);
  if (totalRows == 0)
    Serial.println("BENCH   nothing is waiting to upload - the card is genuinely empty");
  else
    Serial.println("BENCH   these rows have NOT reached the cloud yet - do not erase this card");

  Serial.println("BENCH --- wifi.jsn ---");
  File w = SD.open("wifi.jsn", FILE_READ);
  if (!w) Serial.println("BENCH   absent");
  else
  {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, w);
    w.close();
    if (err) Serial.println("BENCH   present but UNPARSABLE");
    else
    {
      JsonArray a = doc.as<JsonArray>();
      Serial.print("BENCH   entries=");
      Serial.println(a.size());
      for (JsonObject o : a) { Serial.print("BENCH     ssid=["); Serial.print(o["ssid"].as<String>()); Serial.println("]"); }
    }
  }
  Serial.println("BENCH RESULT=OK inventory complete, nothing written");
}

void setup()
{
  Serial.begin(9600);
  { unsigned long _t0 = millis(); while (!Serial && millis() - _t0 < 3000) { ; } }
  delay(1500);
}

// repeat the whole report so it can be caught whenever a capture attaches
void loop()
{
  report();
  delay(15000);
}
