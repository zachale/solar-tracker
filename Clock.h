#ifndef CM_H
#define CM_H

#include <RTClib.h>

class ClockModule {
  public:
    ClockModule();
    bool alarmTriggered();
    int getDayCompletionPercent();
    int getHalfDayExtensionPercent();
    int getHourlyExtensionPercent();
    bool isActiveHours() ;
  private:
    static bool isAlarmTriggered;
    RTC_DS3231 rtc;
    const int RTC_POWER_PIN = 8;
    const int CLOCK_INTERRUPT_PIN = 3;
    const int HALF_DAY_PERCENT = 50; // 50 Percent to describe 1/2 of a day
    float hourStart = 8;
    float hourFinish = 21;
    float percentStart = 20;
    float percentFinish = 80;
    static void setAlarmTriggered();
    uint8_t getHour();
    int getPercentOfDay(float hour);
    int normalizePercentage(float percent);
  };
#endif