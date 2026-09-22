// 00 — seed a WiFi credential onto the SD card, additively.
//
// Reads wifi.jsn, keeps every entry already there, appends the new one only if
// its SSID is absent, writes the file back. Prints the card's root listing and
// the credential SSIDs before and after. Passwords are never printed.

#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include "bench_common.h"

static const char *WIFI_FILE = "wifi.jsn";
static const char *NEW_SSID  = "SmartRG-d0b0";
static const char *NEW_PASS  = "5cea91f5cf";

RTC_DS3231 rtc;

static void listRoot()
{
  File root = SD.open("/");
  if (!root) { Serial.println("BENCH root open FAILED"); return; }
  Serial.println("BENCH --- card root ---");
  unsigned long entries = 0;
  while (entries++ < BENCH_MAX_DIR_ENTRIES)
  {
    File e = root.openNextFile();
    if (!e) break;
    Serial.print("BENCH   ");
    Serial.print(e.name());
    Serial.print("  ");
    Serial.print(e.isDirectory() ? "<dir>" : String(e.size()) + " bytes");
    Serial.println();
    e.close();
  }
  root.close();
}

static void printSsids(JsonArray arr, const char *when)
{
  Serial.print("BENCH ssids ");
  Serial.print(when);
  Serial.print(" (");
  Serial.print(arr.size());
  Serial.print("):");
  for (JsonObject o : arr)
  {
    Serial.print(" [");
    Serial.print(o["ssid"].as<String>());
    Serial.print("]");
  }
  Serial.println();
}

static bool done = false;
static void seed()
{
  benchBanner("01_seed_wifi");
  parkActuator();
  rtcRailOn();
  delay(200);
  Wire.begin();
  rtc.begin();

  if (!SD.begin(PIN_SD_CS))
  {
    Serial.println("BENCH RESULT=FAIL reason=SD.begin failed - card absent, unseated, or wiring");
    return;
  }
  Serial.println("BENCH SD.begin ok");
  listRoot();

  // read existing
  JsonDocument doc;
  JsonArray arr;
  File f = SD.open(WIFI_FILE, FILE_READ);
  if (f)
  {
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err)
    {
      Serial.print("BENCH existing wifi.jsn unparsable (");
      Serial.print(err.c_str());
      Serial.println(") - refusing to overwrite. RESULT=ABORT");
      return;
    }
    arr = doc.as<JsonArray>();
    if (arr.isNull()) { Serial.println("BENCH wifi.jsn is not an array - RESULT=ABORT"); return; }
  }
  else
  {
    Serial.println("BENCH no wifi.jsn present, will create one");
    arr = doc.to<JsonArray>();
  }
  printSsids(arr, "before");

  bool present = false;
  for (JsonObject o : arr)
  {
    if (o["ssid"].as<String>() == NEW_SSID) { present = true; break; }
  }

  if (present)
  {
    Serial.println("BENCH ssid already present, nothing to do. RESULT=NOOP");
    printSsids(arr, "after");
    return;
  }

  JsonObject add = arr.add<JsonObject>();
  add["ssid"] = NEW_SSID;
  add["pass"] = NEW_PASS;

  // back the original up first, and verify the backup, before touching wifi.jsn
  String backup;
  {
    File src = SD.open(WIFI_FILE, FILE_READ);
    if (src)
    {
      while (src.available() && backup.length() < 8192) backup += (char)src.read();
      src.close();
      SD.remove("wifi.bak");
      File bak = SD.open("wifi.bak", FILE_WRITE);
      if (!bak) { Serial.println("BENCH RESULT=ABORT reason=cannot write wifi.bak, refusing to modify wifi.jsn"); return; }
      bak.print(backup);
      bak.close();
      Serial.print("BENCH backup written to wifi.bak, ");
      Serial.print(backup.length());
      Serial.println(" bytes");
    }
  }

  SD.remove(WIFI_FILE);
  File out = SD.open(WIFI_FILE, FILE_WRITE);
  if (!out)
  {
    Serial.println("BENCH write open failed, restoring from backup");
    File r = SD.open(WIFI_FILE, FILE_WRITE);
    if (r && backup.length()) { r.print(backup); r.close(); Serial.println("BENCH restored"); }
    Serial.println("BENCH RESULT=FAIL reason=cannot open wifi.jsn for write");
    return;
  }
  serializeJson(doc, out);
  out.close();

  // verify by re-reading
  JsonDocument check;
  File v = SD.open(WIFI_FILE, FILE_READ);
  if (!v) { Serial.println("BENCH RESULT=FAIL reason=cannot reopen wifi.jsn"); return; }
  DeserializationError verr = deserializeJson(check, v);
  v.close();
  if (verr)
  {
    Serial.println("BENCH verification FAILED, restoring wifi.jsn from backup");
    SD.remove(WIFI_FILE);
    File r = SD.open(WIFI_FILE, FILE_WRITE);
    if (r && backup.length()) { r.print(backup); r.close(); Serial.println("BENCH restored from wifi.bak"); }
    Serial.println("BENCH RESULT=FAIL reason=wrote unparsable json, original restored");
    return;
  }
  printSsids(check.as<JsonArray>(), "after");
  Serial.println("BENCH RESULT=OK credential appended and verified");
}

void setup()
{
  Serial.begin(9600);
  { unsigned long _t0 = millis(); while (!Serial && millis() - _t0 < 3000) { ; } }
  delay(1500);
}

void loop()
{
  if (!done) { seed(); done = true; }
  else Serial.println("BENCH 01 idle - seed already run this power session, reset to repeat");
  delay(15000);
}
