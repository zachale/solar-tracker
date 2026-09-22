// 05 — everything at once. The deployed firmware's exact pattern, accelerated.
//
// Reads the clock, writes the card, and cycles the radio, with RTC sampling
// throughout. This is the closest reproduction of the field workload, run at
// roughly two orders of magnitude the field rate.

#include <WiFiS3.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#define BENCH_WIFI
#include "bench_common.h"
#include "secrets.h"

RTC_DS3231 rtc;
static const char *OUT = "bench05.csv";
static const size_t MAX_BYTES = 200000;
static unsigned long iter = 0, writes = 0, cycles = 0, badCount = 0, firstBadMs = 0, lastReport = 0, lastSpool = 0;
static String ssid, pass;
static bool sdUp = false;

static void sample(const char *phase)
{
  RtcSnap s = rtcSnapshot(rtc);
  iter++;
  if (rtcBad(s))
  {
    badCount++;
    if (!firstBadMs)
    {
      firstBadMs = millis();
      char e[96];
      snprintf(e, sizeof(e), "EVENT=FIRST_ANOMALY phase=%s cycle=%lu writes=%lu", phase, cycles, writes);
      logSnap("05", iter, s, e);
    }
  }
  // spool at the firmware's cadence relative to this accelerated loop
  if (sdUp && millis() - lastSpool > 250)
  {
    lastSpool = millis();
    File f = SD.open(OUT, FILE_WRITE);
    if (f)
    {
      if (f.size() < MAX_BYTES) { f.print(s.unixTime); f.print(','); f.print(s.tempC); f.print(','); f.print(iter); f.print('\n'); writes++; }
      f.close();
    }
  }
}

static void sampleFor(const char *phase, unsigned long ms)
{
  unsigned long t0 = millis();
  while (millis() - t0 < ms) { sample(phase); delay(20); }
}

static bool loadFirstCredential()
{
  File f = SD.open("wifi.jsn", FILE_READ);
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  for (JsonObject o : doc.as<JsonArray>()) { ssid = o["ssid"].as<String>(); pass = o["pass"].as<String>(); return true; }
  return false;
}

static unsigned long runStart = 0;

void setup()
{
  Serial.begin(9600);
  { unsigned long _t0 = millis(); while (!Serial && millis() - _t0 < 3000) { ; } }
  delay(1500);
  benchBanner("05_combined");
  parkActuator();
  rtcRailOn();
  delay(200);
  Wire.begin();
  if (!rtc.begin()) Serial.println("BENCH WARN rtc.begin() false at boot");
  sdUp = SD.begin(PIN_SD_CS);
  Serial.print("BENCH SD.begin "); Serial.println(sdUp ? "ok" : "FAILED");
  if (sdUp) benchSafeRemove(OUT);
  if (!loadFirstCredential()) Serial.println("BENCH WARN no credential on card");
  else { Serial.print("BENCH using ssid ["); Serial.print(ssid); Serial.println("] (password not printed)"); }
  runStart = millis();
  Serial.println("BENCH plan: RTC + SD writes every 250ms + radio cycles, continuously");
}

void loop()
{
  if (benchExpired(runStart))
  {
    Serial.println("BENCH EVENT=RUN_CAP reached, stress halted; reset to run again");
    delay(30000);
    return;
  }
  cycles++;
  Serial.print("BENCH EVENT=CYCLE_START n="); Serial.println(cycles);

  sampleFor("idle", 2000);

  benchConnectAny();
  bool up = (WiFi.status() == WL_CONNECTED);
  Serial.print("BENCH assoc "); Serial.println(up ? "ok" : "FAILED");

  if (up)
  {
    WiFiSSLClient client;
    if (client.connect(apiHost(API).c_str(), 443))
    {
      client.print("GET /time/est HTTP/1.1\r\nHost: "); client.print(apiHost(API)); client.print("\r\nConnection: close\r\n\r\n");
      unsigned long r0 = millis();
      while (client.connected() && millis() - r0 < 10000) { benchDrain(client);sample("http_read"); delay(20); }
      client.stop();
    }
  }

  sampleFor("pre_end", 500);
  WiFi.end();
  Serial.println("BENCH EVENT=WIFI_END");
  sampleFor("post_end", 3000);

  if (millis() - lastReport >= 10000)
  {
    lastReport = millis();
    RtcSnap s = rtcSnapshot(rtc);
    char e[128];
    snprintf(e, sizeof(e), "EVENT=HEARTBEAT cycles=%lu writes=%lu bad=%lu first_bad_ms=%lu", cycles, writes, badCount, firstBadMs);
    logSnap("05", iter, s, e);
  }
}
