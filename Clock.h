#ifndef CM_H
#define CM_H

#include <RTClib.h>

class ClockModule {
  public:
    ClockModule();
    bool alarmTriggered();
    static const void setAlarmTriggered();
    uint8_t getHour();
    int getPercentOfDay(float hour);
    int getDayCompletionPercent();
    int getHalfDayExtensionPercent();
    int getHourlyExtensionPercent();
    int normalizePercentage(float percent);
  private:
    RTC_DS3231 rtc;
    const int RTC_POWER_PIN = 8;
    const int HALF_DAY_PERCENT = 50; // 50 Percent to describe 1/2 of a day
    static bool isAlarmTriggered = false;
    float hourStart = 8;
    float hourFinish = 21;
    float percentStart = 20;
    float percentFinish = 80;
  };
#endif