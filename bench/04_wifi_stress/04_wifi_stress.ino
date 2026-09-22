// 03 — RTC reads interleaved with radio work.
//
// Exercises the transient the deployed build newly added: associate, do a real
// TLS GET, then WiFi.end() (which 71b63aa introduced and the last known-good
// build never called). Read-only against the server: GET /time/est and nothing
// else. Set BENCH_ALLOW_POST to 1 only if you want test rows written into the
// production telemetry bucket.

#include <WiFiS3.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#define BENCH_WIFI
#include "bench_common.h"
#include "secrets.h"

#define BENCH_ALLOW_POST 0

RTC_DS3231 rtc;
static unsigned long iter = 0, cycles = 0, assocFail = 0, getFail = 0, badCount = 0, firstBadMs = 0;
static unsigned long lastReport = 0;
static String ssid, pass;

// read the first credential off the card so nothing is compiled in
static bool loadFirstCredential()
{
  if (!SD.begin(PIN_SD_CS)) { Serial.println("BENCH SD.begin FAILED, cannot read wifi.jsn"); return false; }
  File f = SD.open("wifi.jsn", FILE_READ);
  if (!f) { Serial.println("BENCH wifi.jsn missing"); return false; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.println("BENCH wifi.jsn unparsable"); return false; }
  for (JsonObject o : doc.as<JsonArray>())
  {
    ssid = o["ssid"].as<String>();
    pass = o["pass"].as<String>();
    Serial.print("BENCH using ssid [");
    Serial.print(ssid);
    Serial.println("] (password not printed)");
    return true;
  }
  return false;
}

// poll the RTC while a blocking-ish operation runs, so we sample during the transient
static void sampleDuring(const char *phase, unsigned long ms)
{
  unsigned long t0 = millis();
  while (millis() - t0 < ms)
  {
    RtcSnap s = rtcSnapshot(rtc);
    iter++;
    if (rtcBad(s))
    {
      badCount++;
      if (firstBadMs == 0)
      {
        firstBadMs = millis();
        char e[96];
        snprintf(e, sizeof(e), "EVENT=FIRST_ANOMALY phase=%s cycle=%lu", phase, cycles);
        logSnap("04", iter, s, e);
      }
    }
    delay(20);
  }
}

static unsigned long runStart = 0;

void setup()
{
  Serial.begin(9600);
  { unsigned long _t0 = millis(); while (!Serial && millis() - _t0 < 3000) { ; } }
  delay(1500);
  benchBanner("04_wifi_stress");
  parkActuator();
  rtcRailOn();
  delay(200);
  Wire.begin();
  if (!rtc.begin()) Serial.println("BENCH WARN rtc.begin() false at boot");
  if (!loadFirstCredential()) Serial.println("BENCH WARN no credential, association will fail");
  runStart = millis();
  Serial.println("BENCH plan: repeat { associate, GET /time/est, WiFi.end() }, sampling RTC throughout");
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
  Serial.print("BENCH EVENT=CYCLE_START n=");
  Serial.println(cycles);

  sampleDuring("idle_pre", 1000);

  int st = benchConnectAny();
  {
    RtcSnap s = rtcSnapshot(rtc); iter++;
    if (rtcBad(s)) { badCount++; if (!firstBadMs) { firstBadMs = millis(); logSnap("04", iter, s, "EVENT=FIRST_ANOMALY phase=associating"); } }
    delay(100);
  }
  bool up = (WiFi.status() == WL_CONNECTED);
  if (!up) assocFail++;
  Serial.print("BENCH assoc ");
  Serial.print(up ? "ok" : "FAILED");
  Serial.print(" rssi=");
  Serial.println(up ? WiFi.RSSI() : 0);

  sampleDuring(up ? "associated" : "assoc_failed", 2000);

  if (up)
  {
    WiFiSSLClient client;
    if (client.connect(apiHost(API).c_str(), 443))
    {
      client.print("GET /time/est HTTP/1.1\r\nHost: ");
      client.print(apiHost(API));
      client.print("\r\nConnection: close\r\n\r\n");
      unsigned long r0 = millis();
      while (client.connected() && millis() - r0 < 10000)
      {
        benchDrain(client);
        RtcSnap s = rtcSnapshot(rtc); iter++;
        if (rtcBad(s)) { badCount++; if (!firstBadMs) { firstBadMs = millis(); logSnap("04", iter, s, "EVENT=FIRST_ANOMALY phase=http_read"); } }
        delay(20);
      }
      client.stop();
      Serial.println("BENCH GET /time/est done");
    }
    else { getFail++; Serial.println("BENCH GET connect FAILED"); }
  }

  sampleDuring("pre_end", 500);
  WiFi.end();                       // the call 71b63aa added
  Serial.println("BENCH EVENT=WIFI_END");
  sampleDuring("post_end", 3000);   // the window we most suspect

  if (millis() - lastReport >= 10000)
  {
    lastReport = millis();
    RtcSnap s = rtcSnapshot(rtc);
    char e[128];
    snprintf(e, sizeof(e), "EVENT=HEARTBEAT cycles=%lu assocfail=%lu getfail=%lu bad=%lu first_bad_ms=%lu",
             cycles, assocFail, getFail, badCount, firstBadMs);
    logSnap("04", iter, s, e);
  }
}
