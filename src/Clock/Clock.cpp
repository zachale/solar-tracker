#include "./Clock.h"

bool ClockModule::alarmTriggered = false;

void ClockModule::setup()
{

  pinMode(RTC_POWER_PIN, OUTPUT);

  powerCycle();

  if (!rtc.begin())
  {
    while (1)
      delay(10);
  }

  rtc.disable32K();

  rtc.clearAlarm(1);
  rtc.clearAlarm(2);
  rtc.writeSqwPinMode(DS3231_OFF);
  rtc.disableAlarm(2);

  if (rtc.lostPower())
  {
    Serial.println("RTC lost power, let's set the time!");
  }

  resetAlarm();

  Serial.println("Setup Clock Module.");
}

bool ClockModule::powerCycle()
{
  digitalWrite(RTC_POWER_PIN, LOW);
  for (int i = 0; i < 5; i++)
  {
    digitalWrite(RTC_POWER_PIN, HIGH);
    if (rtc.begin())
    {
      return true;
    }
    digitalWrite(RTC_POWER_PIN, LOW);
    Serial.println("Couldn't find RTC");
    delay(5000);
  }
  return false;
}

bool ClockModule::isAlarmTriggered()
{
  if (!alarmTriggered)
  {
    return false;
  }

  alarmTriggered = false;
  resetAlarm();
}

void ClockModule::resetAlarm()
{
  rtc.clearAlarm(1);
  Serial.println("Alarm detected and cleared.");
  if (!rtc.setAlarm1(
          DateTime(2024, 10, 1, 8, 0, 0),
          DS3231_A1_Minute))
  {
    Serial.println("Error, alarm wasn't set!");
  }
  else
  {
    Serial.println("Alarm was set.");
  }
}

void ClockModule::setAlarmTriggered()
{
  alarmTriggered = true;
}

DateTime ClockModule::now()
{
  DateTime currentTime = rtc.now();
  // If the clock returns this time, it has malfunctioned and needs to be power cycled
  if (currentTime.timestamp() == String(1332900840))
  {
    Serial.println("RTC Malfunctioned! Power cycling.");
    Serial.println("Lost Power: " + String(rtc.lostPower()));
    Serial.println("Temperature: " + String(rtc.getTemperature()));
    Serial.println("TimeStamp: " + String(getTimestamp()));
    Serial.println("Time: " + String(getFullTimeString()));
    powerCycle();
  }
  return currentTime;
}

uint8_t ClockModule::hour()
{
  return now().hour();
}

uint8_t ClockModule::minute()
{
  return now().minute();
}

uint8_t ClockModule::second()
{
  return now().second();
}

int ClockModule::getPercentOfDay(float hour)
{
  return (hour - hourStart) / (hourFinish - hourStart) * 100;
}

int ClockModule::getDayCompletionPercent()
{
  uint8_t currentHour = hour();
  return getPercentOfDay(currentHour);
}

int ClockModule::getHalfDayExtensionPercent()
{
  return normalizePercentage(HALF_DAY_PERCENT);
}

int ClockModule::getHourlyExtensionPercent()
{
  float dayCompletePercent = getDayCompletionPercent();
  return normalizePercentage(dayCompletePercent);
}

// Normalises a percentage calculated between 'working hours' to an actuator extension percentage
int ClockModule::normalizePercentage(float percentDayComplete)
{
  float startPercentExtended = percentMiddle;
  float finishPercentExtended = percentFinish;
  int startHour = hourMiddle;
  int finishHour = hourFinish;

  if (percentDayComplete < getPercentOfDay(hourMiddle))
  {
    startPercentExtended = percentStart;
    finishPercentExtended = percentMiddle;
    startHour = hourStart;
    finishHour = hourMiddle;
  }

  float intervalizedPercent = mapPercentToInterval(startHour, finishHour, percentDayComplete);
  float difference = finishPercentExtended - startPercentExtended;
  return (difference * (intervalizedPercent / 100)) + startPercentExtended;
}

float ClockModule::mapPercentToInterval(float intervalStartHour, float intervalEndHour, float percent)
{
  return (percent - getPercentOfDay(intervalStartHour)) * 100 / (getPercentOfDay(intervalEndHour) - getPercentOfDay(intervalStartHour));
}

bool ClockModule::isActiveHours()
{
  return (hour() >= hourStart && hour() <= hourFinish);
}

float ClockModule::getClockTemp()
{
  return rtc.getTemperature();
}

String ClockModule::getSimpleTimeString()
{
  String result = "";
  result.concat(hour());
  result.concat(":");
  result.concat(minute());
  return result;
}

String ClockModule::getFullTimeString()
{
  String result = "";
  result.concat(hour());
  result.concat(":");
  result.concat(minute());
  result.concat(":");
  result.concat(second());
  return result;
}

uint32_t ClockModule::getTimestamp()
{
  return now().unixtime();
}

void ClockModule::setDateTime(const char *dateTimeString)
{
  rtc.adjust(DateTime(dateTimeString));
}

void ClockModule::setSimpleTime(int hour, int minute)
{
  uint16_t year = now().year();
  uint8_t month = now().month();
  uint8_t day = now().day();
  rtc.adjust(DateTime(year, month, day, hour, minute, 0));
}

void ClockModule::setSchedule(JsonDocument &doc)
{
  hourStart = doc["setStartHour"].as<float>();
  hourMiddle = doc["setMiddleHour"].as<float>();
  hourFinish = doc["setEndHour"].as<float>();
  percentStart = doc["setStartPercent"].as<float>();
  percentMiddle = doc["setMiddlePercent"].as<float>();
  percentFinish = doc["setEndPercent"].as<float>();
}

String ClockModule::toHtml()
{
  String html;
  html.concat(R"(
    <h1 style="font-size:7vw;">Clock</h1>
    <section>
      <form action="/" method="POST">
        Set time (HH:MM - 24 hour format): 
        <input style="font-size:5vw;" type="text" pattern="^([01]\d|2[0-3]):([0-5]\d)$" name="setTime" value=")");
  html.concat(getSimpleTimeString());
  html.concat(R"(">
        <input type="submit" style="font-size:5vw;" value="Submit">      
      </form>
    </section>
  )");

  html.concat(R"(
    <h1 style="font-size:7vw;">Schedule</h1>
    <section>
      <form action="/" method="POST">
        <br/> 
        Set start hour (HH): 
        <input style="font-size:5vw;" type="number" pattern="^([1-9]|1[0-9]|2[0-4])$" name="setStartHour" value=")");
  html.concat((int)hourStart);
  html.concat(R"(">
        at 
        <input style="font-size:5vw;" type="number" max="100" min="0" name="setStartPercent" value=")");
  html.concat((int)percentStart);
  html.concat(R"(">
        <br/> 
        Set middle hour (HH): 
        <input style="font-size:5vw;" type="number" pattern="^([1-9]|1[0-9]|2[0-4])$" name="setMiddleHour" value=")");
  html.concat((int)hourMiddle);
  html.concat(R"(">
        at 
        <input style="font-size:5vw;" type="number" max="100" min="0" name="setMiddlePercent" value=")");
  html.concat((int)percentMiddle);
  html.concat(R"(">
        <br/> 
        Set end hour (HH): 
        <input style="font-size:5vw;" type="number" pattern="^([1-9]|1[0-9]|2[0-4])$" name="setEndHour" value=")");
  html.concat((int)hourFinish);
  html.concat(R"(">
        at 
        <input style="font-size:5vw;" type="number" max="100" min="0" name="setEndPercent" value=")");
  html.concat((int)percentFinish);
  html.concat(R"(">
        <input type="submit" style="font-size:5vw;" value="Submit">    
      </form>
    </section>
  )");
  return html;
}