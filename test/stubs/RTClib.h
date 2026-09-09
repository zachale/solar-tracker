// Host stand-in for RTClib. The DateTime arithmetic is real (it has to be — the
// schedule logic depends on it); the DS3231 itself is backed by a settable clock
// so tests can put the tracker at any moment of any day.
#pragma once

#include <Arduino.h>
#include <ctime>

enum Ds3231SqwPinMode { DS3231_OFF, DS3231_SquareWave1Hz, DS3231_SquareWave1kHz };
enum Ds3231Alarm1Mode {
  DS3231_A1_PerSecond, DS3231_A1_Second, DS3231_A1_Minute,
  DS3231_A1_Hour, DS3231_A1_Date, DS3231_A1_Day
};
enum Ds3231Alarm2Mode { DS3231_A2_PerMinute, DS3231_A2_Minute, DS3231_A2_Hour };

class DateTime {
public:
  DateTime() : t(0) {}
  DateTime(uint32_t unix) : t(unix) {}
  DateTime(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    t = (uint32_t)timegm(&tm);
  }
  // Parses "YYYY-MM-DDTHH:MM:SS" (trailing zone designator ignored, as RTClib does).
  DateTime(const char *iso) {
    int y = 2000, mo = 1, d = 1, h = 0, mi = 0, s = 0;
    if (iso) std::sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s);
    *this = DateTime((uint16_t)y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, (uint8_t)s);
  }

  uint16_t year() const   { return (uint16_t)(brk().tm_year + 1900); }
  uint8_t  month() const  { return (uint8_t)(brk().tm_mon + 1); }
  uint8_t  day() const    { return (uint8_t)brk().tm_mday; }
  uint8_t  hour() const   { return (uint8_t)brk().tm_hour; }
  uint8_t  minute() const { return (uint8_t)brk().tm_min; }
  uint8_t  second() const { return (uint8_t)brk().tm_sec; }
  uint32_t unixtime() const { return t; }

private:
  uint32_t t;
  std::tm brk() const {
    std::time_t tt = (std::time_t)t;
    std::tm out{};
    gmtime_r(&tt, &out);
    return out;
  }
};

class RTC_DS3231 {
public:
  // test hooks
  DateTime mockNow = DateTime(2026, 9, 9, 10, 0, 0);
  bool mockLostPower = false;
  bool mockBeginOk = true;
  float mockTemp = 22.0f;
  int alarmsCleared = 0;
  int alarmsSet = 0;

  bool begin() { return mockBeginOk; }
  bool lostPower() { return mockLostPower; }
  DateTime now() { return mockNow; }
  void adjust(const DateTime &d) { mockNow = d; mockLostPower = false; }
  float getTemperature() { return mockTemp; }
  void writeSqwPinMode(Ds3231SqwPinMode) {}
  bool setAlarm1(const DateTime &, Ds3231Alarm1Mode) { alarmsSet++; return true; }
  bool setAlarm2(const DateTime &, Ds3231Alarm2Mode) { alarmsSet++; return true; }
  void clearAlarm(uint8_t) { alarmsCleared++; }
  void disableAlarm(uint8_t) {}
  void disable32K() {}
};
