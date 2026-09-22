// 02 — RTC reads interleaved with SD writes, at a ramping rate.
//
// The deployed firmware writes one row every ten minutes. This writes one every
// few hundred milliseconds and tightens the interval every 60s, so an hour here
// is worth months of field duty. Writes go to bench02.csv only; telemetry
// chunks (tlm*.csv), config and wifi.jsn are never touched.

#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include "bench_common.h"

RTC_DS3231 rtc;
static const char *OUT = "bench02.csv";
static const size_t MAX_BYTES = 200000;      // stop growing the file past ~200 KB

static unsigned long iter = 0, writes = 0, writeFails = 0, badCount = 0, firstBadMs = 0;
static unsigned long stageStart = 0, lastReport = 0;
static unsigned long periodMs = 500;          // ramps 500 -> 250 -> 125 -> 60 -> 30
static int stage = 0;
static bool sdUp = false;

static unsigned long runStart = 0;

void setup()
{
  Serial.begin(9600);
  { unsigned long _t0 = millis(); while (!Serial && millis() - _t0 < 3000) { ; } }
  delay(1500);
  benchBanner("03_sd_stress");
  parkActuator();
  rtcRailOn();
  delay(200);
  Wire.begin();
  if (!rtc.begin()) Serial.println("BENCH WARN rtc.begin() false at boot");
  sdUp = SD.begin(PIN_SD_CS);
  Serial.print("BENCH SD.begin ");
  Serial.println(sdUp ? "ok" : "FAILED");
  benchSafeRemove(OUT);
  stageStart = millis();
  runStart = millis();
  Serial.println("BENCH plan: RTC read then SD write, period 500ms halving every 60s, 5 stages");
}

void loop()
{
  if (benchExpired(runStart))
  {
    Serial.println("BENCH EVENT=RUN_CAP reached, stress halted; reset to run again");
    delay(30000);
    return;
  }
  // stage ramp
  if (millis() - stageStart > 60000 && stage < 4)
  {
    stage++;
    periodMs = (stage == 4) ? 30 : periodMs / 2;
    stageStart = millis();
    Serial.print("BENCH EVENT=STAGE stage=");
    Serial.print(stage);
    Serial.print(" period_ms=");
    Serial.println(periodMs);
  }

  RtcSnap before = rtcSnapshot(rtc);
  iter++;

  // the firmware's exact pattern: read the clock, then immediately write the card
  if (sdUp)
  {
    File f = SD.open(OUT, FILE_WRITE);
    if (!f)
    {
      writeFails++;
    }
    else
    {
      if (f.size() < MAX_BYTES)
      {
        f.print(before.unixTime); f.print(',');
        f.print(before.tempC);    f.print(',');
        f.print(iter);            f.print('\n');
        writes++;
      }
      f.close();
    }
  }

  RtcSnap after = rtcSnapshot(rtc);

  if (rtcBad(after) || rtcBad(before))
  {
    badCount++;
    if (firstBadMs == 0)
    {
      firstBadMs = millis();
      char e[96];
      snprintf(e, sizeof(e), "EVENT=FIRST_ANOMALY when=%s period_ms=%lu writes=%lu",
               rtcBad(before) ? "before_write" : "after_write", periodMs, writes);
      logSnap("03", iter, after, e);
    }
  }

  if (millis() - lastReport >= 10000)
  {
    lastReport = millis();
    char e[128];
    snprintf(e, sizeof(e), "EVENT=HEARTBEAT period_ms=%lu writes=%lu wfail=%lu bad=%lu first_bad_ms=%lu",
             periodMs, writes, writeFails, badCount, firstBadMs);
    logSnap("03", iter, after, e);
  }
  delay(periodMs);
}
