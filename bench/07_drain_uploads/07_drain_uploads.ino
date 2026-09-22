// 07 — upload the telemetry chunks stranded on the SD card.
//
// Mirrors SolarTracker::uploadLogs() exactly: read chunk, POST text/csv to
// /data, and clear the chunk ONLY after a confirmed 200. On any other response
// it stops and keeps every remaining chunk. Nothing is deleted speculatively.

#include <WiFiS3.h>
#include <R4HttpClient.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include "bench_common.h"
#include "secrets.h"

RTC_DS3231 rtc;
WiFiSSLClient netClient;
R4HttpClient http;

static const int MAX_CHUNKS = 64;
static const size_t CHUNK_BYTES = 4096;
static bool done = false;
static unsigned long uploaded = 0, rowsSent = 0, failed = 0;

static String chunkPath(int i) { return "tlm" + String(i) + ".csv"; }

static size_t chunkSize(int i)
{
  File f = SD.open(chunkPath(i).c_str(), FILE_READ);
  if (!f) return 0;
  size_t n = f.size();
  f.close();
  return n;
}

static String readChunk(int i)
{
  File f = SD.open(chunkPath(i).c_str(), FILE_READ);
  if (!f) return "";
  size_t bytes = f.size();
  if (bytes > CHUNK_BYTES) bytes = CHUNK_BYTES;
  String out;
  out.reserve(bytes + 1);
  for (size_t k = 0; k < bytes; k++) { int c = f.read(); if (c < 0) break; out += (char)c; }
  f.close();
  return out;
}

static unsigned long countRows(const String &s)
{
  unsigned long n = 0;
  for (unsigned int i = 0; i < s.length(); i++) if (s[i] == '\n') n++;
  return n;
}

static bool connectWifi()
{
  if (WiFi.status() == WL_CONNECTED) return true;
  File f = SD.open("wifi.jsn", FILE_READ);
  if (!f) { Serial.println("BENCH no wifi.jsn"); return false; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.println("BENCH wifi.jsn unparsable"); return false; }

  WiFi.end();
  for (JsonObject o : doc.as<JsonArray>())
  {
    String ssid = o["ssid"].as<String>();
    Serial.print("BENCH trying ssid [");
    Serial.print(ssid);
    Serial.println("]");
    WiFi.begin(ssid.c_str(), o["pass"].as<String>().c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.print("BENCH associated, rssi=");
      Serial.println(WiFi.RSSI());
      return true;
    }
    Serial.println("BENCH   association failed");
  }
  return false;
}

static void drain()
{
  RtcSnap s = rtcSnapshot(rtc);
  logSnap("07", 0, s, "EVENT=RTC_BEFORE_DRAIN");

  if (!SD.begin(PIN_SD_CS)) { Serial.println("BENCH RESULT=FAIL reason=SD.begin failed"); return; }

  int pending = 0;
  unsigned long pendingRows = 0;
  for (int i = 0; i < MAX_CHUNKS; i++)
  {
    size_t n = chunkSize(i);
    if (n > 0) { pending++; pendingRows += countRows(readChunk(i)); }
  }
  Serial.print("BENCH pending chunks=");
  Serial.print(pending);
  Serial.print(" rows=");
  Serial.println(pendingRows);
  if (pending == 0) { Serial.println("BENCH RESULT=NOOP nothing to upload"); return; }

  if (!connectWifi()) { Serial.println("BENCH RESULT=FAIL reason=no wifi, NOTHING deleted"); return; }

  // Probe with the small GET the firmware does on every sync. If this succeeds
  // and the 4 KB POST does not, the failure is size/transfer related, not
  // connectivity.
  {
    Serial.print("BENCH rssi=");
    Serial.println(WiFi.RSSI());
    String gurl = apiUrl(API, "/time/est");
    http.begin(netClient, gurl, 443);
    http.setTimeout(15000);
    http.addHeader("User-Agent: Arduino UNO R4 WiFi");
    int gc = http.GET();
    Serial.print("BENCH PROBE GET /time/est -> ");
    Serial.println(gc);
    http.close();
    delay(500);
  }

  for (int i = 0; i < MAX_CHUNKS; i++)
  {
    if (chunkSize(i) == 0) continue;
    String body = readChunk(i);
    if (body.length() == 0) continue;
    unsigned long rows = countRows(body);

    Serial.print("BENCH uploading chunk ");
    Serial.print(i);
    Serial.print(" bytes=");
    Serial.print(body.length());
    Serial.print(" rows=");
    Serial.println(rows);

    String url = apiUrl(API, "/data");
    int code = 0;
    for (int attempt = 1; attempt <= 3; attempt++)
    {
      http.begin(netClient, url, 443);
      http.setTimeout(15000);
      http.addHeader("User-Agent: Arduino UNO R4 WiFi");
      http.addHeader("Content-Type: text/csv");
      code = http.POST(body);
      http.close();
      Serial.print("BENCH   attempt ");
      Serial.print(attempt);
      Serial.print(" response=");
      Serial.print(code);
      Serial.print(" rssi=");
      Serial.print(WiFi.RSSI());
      Serial.print(" ram=");
      Serial.println(freeRam());
      if (code == 200) break;
      delay(2000);
    }

    if (code == 200)
    {
      SD.remove(chunkPath(i).c_str());   // ONLY after a confirmed 200
      uploaded++;
      rowsSent += rows;
      Serial.print("BENCH   chunk ");
      Serial.print(i);
      Serial.println(" cleared");
    }
    else
    {
      failed++;
      Serial.println("BENCH   NOT 200 - keeping this chunk and stopping here");
      break;
    }
    delay(500);
  }

  Serial.print("BENCH RESULT=DONE chunks_uploaded=");
  Serial.print(uploaded);
  Serial.print(" rows_sent=");
  Serial.print(rowsSent);
  Serial.print(" failed=");
  Serial.println(failed);

  RtcSnap after = rtcSnapshot(rtc);
  logSnap("07", 1, after, "EVENT=RTC_AFTER_DRAIN");
}

void setup()
{
  Serial.begin(9600);
  { unsigned long _t0 = millis(); while (!Serial && millis() - _t0 < 3000) { ; } }
  delay(1500);
  benchBanner("07_drain_uploads");
  parkActuator();
  rtcRailOn();
  delay(200);
  Wire.begin();
  rtc.begin();
}

void loop()
{
  if (!done) { drain(); done = true; }
  else { Serial.println("BENCH 07 idle - drain complete, reset to run again"); }
  delay(20000);
}
