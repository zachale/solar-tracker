// 01 — control. RTC only. No SD, no WiFi, no actuator.
//
// Polls the DS3231 as fast as the firmware never does, and reports the first
// anomaly. If the clock fails here, nothing else in the system is implicated.

#include <RTClib.h>
#include "bench_common.h"

RTC_DS3231 rtc;
static unsigned long iter = 0;
static unsigned long firstBadMs = 0;
static unsigned long badCount = 0;
static unsigned long lastReport = 0;
static const unsigned long POLL_MS = 100;   // 10 Hz, ~60x the firmware's rate

static unsigned long runStart = 0;

void setup()
{
  Serial.begin(9600);
  { unsigned long _t0 = millis(); while (!Serial && millis() - _t0 < 3000) { ; } }
  delay(1500);
  benchBanner("02_baseline_rtc");
  parkActuator();
  rtcRailOn();
  delay(200);
  Wire.begin();
  if (!rtc.begin()) Serial.println("BENCH WARN rtc.begin() returned false at boot");
  runStart = millis();
  Serial.println("BENCH plan: poll RTC every 100ms, report every 10s, flag first anomaly");
}

void loop()
{
  if (benchExpired(runStart))
  {
    Serial.println("BENCH EVENT=RUN_CAP reached, stress halted; reset to run again");
    delay(30000);
    return;
  }
  RtcSnap s = rtcSnapshot(rtc);
  iter++;
  if (rtcBad(s))
  {
    badCount++;
    if (firstBadMs == 0)
    {
      firstBadMs = millis();
      logSnap("02", iter, s, "EVENT=FIRST_ANOMALY");
    }
  }

  if (millis() - lastReport >= 10000)
  {
    lastReport = millis();
    char extra[64];
    snprintf(extra, sizeof(extra), "EVENT=HEARTBEAT bad_total=%lu first_bad_ms=%lu", badCount, firstBadMs);
    logSnap("02", iter, s, extra);
  }
  delay(POLL_MS);
}
