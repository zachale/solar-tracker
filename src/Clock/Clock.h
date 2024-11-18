#ifndef CM_H
#define CM_H

#include <RTClib.h>

class ClockModule
{
public:
  void setup();
  static void setAlarmTriggered();
  bool isAlarmTriggered();
  int getDayCompletionPercent();
  int getHalfDayExtensionPercent();
  int getHourlyExtensionPercent();
  bool isActiveHours();
  float getClockTemp();
  String getFullTimeString();
  String getSimpleTimeString();
  uint32_t getTimestamp();
  void setDateTime(const char* dateTimeString);
  void setSimpleTime(int hour, int minute);
  bool requireSync();

private:
  static bool alarmTriggered;
  RTC_DS3231 rtc;
  const int RTC_POWER_PIN = 8;
  const int HALF_DAY_PERCENT = 50; // 50 Percent to describe 1/2 of a day
  float hourStart = 8;
  float hourFinish = 20;
  float percentStart = 20;
  float percentFinish = 80;
  int syncInterval = 86400000; // 24 hours in milliseconds
  unsigned long syncTimer = 0;
  int getPercentOfDay(float hour);
  int normalizePercentage(float percent);
  uint8_t getHour();
  uint8_t getMinute();
  uint8_t getSecond();
  void resetAlarm();
};
#endif