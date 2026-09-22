// 06 — stress until the bus wedges, then find out what actually un-wedges it.
//
// This is the test that produces an answer we can ship. It runs the combined
// workload; the moment the RTC stops answering it climbs a recovery ladder and
// records which rung worked:
//
//   rung 0  re-read            (was it a one-sample transient?)
//   rung 1  Wire.end/begin     (resets the MCU side of the bus)
//   rung 2  rtc.begin()        (what the firmware's retry does)
//   rung 3  D8 rail power cycle (powerCycleRTC - the only thing that can make a
//                               slave holding SDA low let go)
//
// Nothing here is destructive: rung 3 is exactly what ClockModule::powerCycleRTC
// already does in the shipped firmware.

#include <WiFiS3.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#define BENCH_WIFI
#include "bench_common.h"
#include "secrets.h"

RTC_DS3231 rtc;
static const char *OUT = "bench06.csv";
static unsigned long iter = 0, wedges = 0, lastReport = 0, lastSpool = 0, runStart = 0;
static unsigned long rungWins[4] = {0, 0, 0, 0};
static unsigned long unrecovered = 0;
// Without these, a permanently wedged bus would re-enter the ladder on every
// 20 ms sample and power-cycle the RTC rail continuously.
static const unsigned long RECOVERY_COOLDOWN_MS = 30000;
static const unsigned long MAX_RECOVERIES = 40;
static unsigned long lastRecoveryMs = 0;
static bool gaveUp = false;
static String ssid, pass;
static bool sdUp = false;

static bool healthy()
{
  RtcSnap s = rtcSnapshot(rtc);
  return !rtcBad(s);
}

static void attemptRecovery()
{
  wedges++;
  unsigned long t0 = millis();
  RtcSnap at = rtcSnapshot(rtc);
  char e[128];
  snprintf(e, sizeof(e), "EVENT=WEDGE n=%lu uptime_ms=%lu", wedges, millis() - runStart);
  logSnap("06", iter, at, e);

  // rung 0 - just read again
  delay(50);
  if (healthy()) { rungWins[0]++; Serial.print("BENCH EVENT=RECOVERED rung=0 method=reread ms="); Serial.println(millis() - t0); return; }

  // rung 1 - reset the MCU side of the bus
  Wire.end();
  delay(20);
  Wire.begin();
  delay(50);
  if (healthy()) { rungWins[1]++; Serial.print("BENCH EVENT=RECOVERED rung=1 method=wire_end_begin ms="); Serial.println(millis() - t0); return; }

  // rung 2 - what the firmware's retry loop does
  rtc.begin();
  delay(50);
  if (healthy()) { rungWins[2]++; Serial.print("BENCH EVENT=RECOVERED rung=2 method=rtc_begin ms="); Serial.println(millis() - t0); return; }

  // rung 3 - cut power to the slave, the firmware's powerCycleRTC
  Serial.println("BENCH EVENT=RUNG3 cycling D8 rail");
  rtcRailCycle(1000, 1000);
  Wire.end(); delay(20); Wire.begin();
  rtc.begin();
  delay(50);
  if (healthy()) { rungWins[3]++; Serial.print("BENCH EVENT=RECOVERED rung=3 method=rail_power_cycle ms="); Serial.println(millis() - t0); return; }

  unrecovered++;
  Serial.print("BENCH EVENT=UNRECOVERED after all rungs, ms="); Serial.println(millis() - t0);
}

static void sample(const char *phase)
{
  RtcSnap s = rtcSnapshot(rtc);
  iter++;
  if (sdUp && millis() - lastSpool > 250)
  {
    lastSpool = millis();
    File f = SD.open(OUT, FILE_WRITE);
    if (f) { if (f.size() < 200000) { f.print(s.unixTime); f.print(','); f.print(iter); f.print('\n'); } f.close(); }
  }
  if (rtcBad(s))
  {
    if (gaveUp) return;
    if (wedges >= MAX_RECOVERIES)
    {
      gaveUp = true;
      Serial.println("BENCH EVENT=GAVE_UP recovery cap reached, no further rail cycling");
      return;
    }
    if (lastRecoveryMs && millis() - lastRecoveryMs < RECOVERY_COOLDOWN_MS) return;
    lastRecoveryMs = millis();
    attemptRecovery();
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
  if (deserializeJson(doc, f)) { f.close(); return false; }
  f.close();
  for (JsonObject o : doc.as<JsonArray>()) { ssid = o["ssid"].as<String>(); pass = o["pass"].as<String>(); return true; }
  return false;
}

void setup()
{
  Serial.begin(9600);
  { unsigned long _t0 = millis(); while (!Serial && millis() - _t0 < 3000) { ; } }
  delay(1500);
  benchBanner("06_recovery_ladder");
  parkActuator();
  rtcRailOn();
  delay(200);
  Wire.begin();
  rtc.begin();
  sdUp = SD.begin(PIN_SD_CS);
  Serial.print("BENCH SD.begin "); Serial.println(sdUp ? "ok" : "FAILED");
  if (sdUp) benchSafeRemove(OUT);
  loadFirstCredential();
  runStart = millis();
  Serial.println("BENCH plan: combined stress; on every wedge climb rungs 0-3 and record the winner");
}

void loop()
{
  if (benchExpired(runStart))
  {
    Serial.println("BENCH EVENT=RUN_CAP reached, stress halted; reset to run again");
    delay(30000);
    return;
  }
  sampleFor("idle", 2000);
  benchConnectAny();
  if (WiFi.status() == WL_CONNECTED)
  {
    WiFiSSLClient c;
    if (c.connect(apiHost(API).c_str(), 443))
    {
      c.print("GET /time/est HTTP/1.1\r\nHost: "); c.print(apiHost(API)); c.print("\r\nConnection: close\r\n\r\n");
      unsigned long r0 = millis();
      while (c.connected() && millis() - r0 < 10000) { benchDrain(c);sample("http_read"); delay(20); }
      c.stop();
    }
  }
  sampleFor("pre_end", 500);
  WiFi.end();
  sampleFor("post_end", 3000);

  if (millis() - lastReport >= 15000)
  {
    lastReport = millis();
    RtcSnap s = rtcSnapshot(rtc);
    char e[160];
    snprintf(e, sizeof(e), "EVENT=TALLY wedges=%lu rung0=%lu rung1=%lu rung2=%lu rung3=%lu unrecovered=%lu",
             wedges, rungWins[0], rungWins[1], rungWins[2], rungWins[3], unrecovered);
    logSnap("06", iter, s, e);
  }
}
