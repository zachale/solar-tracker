#ifndef CM_H
#define CM_H

#include <RTClib.h>
#include <ArduinoJson.h>
#include "../SettingsManager/SettingsManager.h"
#include <ArduinoJson.h>

class ClockModule
{
public:
  void setup();
  static void setAlarmTriggered();
  bool isAlarmTriggered();
  int getDayCompletionPercent();
  int getSafeExtensionPercent();
  int getHourlyExtensionPercent();
  bool isActiveHours();
  float getClockTemp();
  String getFullTimeString();
  String getSimpleTimeString();
  uint32_t getTimestamp();
  void setDateTime(const char *dateTimeString);
  void setSimpleTime(int hour, int minute);
  String toHtml();
  void setSchedule(JsonDocument &doc);
  uint8_t getHour();

private:
  RTC_DS3231 rtc;
  static bool alarmTriggered;
  const int RTC_POWER_PIN = 8;
  const int HALF_DAY_PERCENT = 50; // 50 Percent to describe 1/2 of a day
  float hourStart = 8;
  float hourMiddle = 12;
  float hourFinish = 20;
  float percentStart = 20;
  float percentMiddle = 66;
  float percentFinish = 80;
  float safeExtensionPercent = 50; // Default safe extension percentage
  int syncInterval = 86400000;     // 24 hours in milliseconds
  unsigned long syncTimer = 0;
  DateTime lastTimeCheck;
  bool wasZeroTime = false;
  int getPercentOfDay(float hour);
  int normalizePercentage(float percent);
  uint8_t getMinute();
  uint8_t getSecond();
  void resetAlarm();
  float mapPercentToInterval(float intervalStartHour, float intervalEndHour, float percent);
  void powerCycleRTC();
  bool checkForZeroTime();
  bool initializeRTC();
};
#endif