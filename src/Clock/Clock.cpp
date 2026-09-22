#include "./Clock.h"

bool ClockModule::alarmTriggered = false;
uint16_t ClockModule::wedgeCount = 0;
uint16_t ClockModule::recoverCount = 0;
uint8_t ClockModule::lastI2cCode = 0;

// A4/A5 on the UNO R4 WiFi. Taken back as plain GPIO only during a bus clear.
static const uint8_t BUS_SDA_PIN = 18;
static const uint8_t BUS_SCL_PIN = 19;

// Does the DS3231 actually answer? This is the detector checkForZeroTime()
// should always have been: the real failure returns non-zero garbage, never
// an all-zero time, so the old test never fired.
bool ClockModule::isHealthy()
{
  Wire.beginTransmission(0x68);
  lastI2cCode = Wire.endTransmission();
  if (lastI2cCode != 0)
  {
    return false;
  }
  DateTime now = rtc.now();
  return now.year() >= 2020 && now.year() <= 2099;
}

// NXP UM10204 3.1.16 bus clear. If a slave was interrupted mid-byte it can sit
// holding SDA low forever; clocking SCL lets it finish the word and release.
// Nothing in the Renesas core does this: its abort function is never called,
// the hardware SCL-low timeout is disabled, and setWireTimeout() discards the
// reset flag. So it has to be done by hand.
bool ClockModule::busClear()
{
  Wire.end();
  delay(10);
  pinMode(BUS_SDA_PIN, INPUT_PULLUP);
  pinMode(BUS_SCL_PIN, INPUT_PULLUP);
  delay(2);

  if (digitalRead(BUS_SDA_PIN) == LOW)
  {
    pinMode(BUS_SCL_PIN, OUTPUT);
    for (int i = 0; i < 9 && digitalRead(BUS_SDA_PIN) == LOW; i++)
    {
      digitalWrite(BUS_SCL_PIN, LOW);
      delayMicroseconds(5);
      digitalWrite(BUS_SCL_PIN, HIGH);
      delayMicroseconds(5);
    }
    // manual STOP: SDA released while SCL is high
    pinMode(BUS_SDA_PIN, OUTPUT);
    digitalWrite(BUS_SDA_PIN, LOW);
    delayMicroseconds(5);
    pinMode(BUS_SCL_PIN, INPUT_PULLUP);
    delayMicroseconds(5);
    pinMode(BUS_SDA_PIN, INPUT_PULLUP);
    delayMicroseconds(5);
  }

  pinMode(BUS_SDA_PIN, INPUT_PULLUP);
  pinMode(BUS_SCL_PIN, INPUT_PULLUP);
  bool freed = digitalRead(BUS_SDA_PIN) == HIGH;
  Wire.begin();
  delay(20);
  rtc.begin();
  return freed;
}

void ClockModule::setup()
{
  pinMode(RTC_POWER_PIN, OUTPUT);
  digitalWrite(RTC_POWER_PIN, HIGH);

  if (!initializeRTC())
  {
    Serial.println("Failed to initialize RTC after multiple attempts");
    return;
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
  lastTimeCheck = rtc.now();
  wasZeroTime = false;

  Serial.println("Setup Clock Module.");

  if (SettingsManager::getSetting<String>("schedule") != "null")
  {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, SettingsManager::getSetting<String>("schedule"));
    if (!error)
    {
      setSchedule(doc);
    }
  }
}

void ClockModule::powerCycleRTC()
{
  Serial.println("Power cycling RTC...");
  digitalWrite(RTC_POWER_PIN, LOW);
  delay(1000); // Wait for power to fully discharge
  digitalWrite(RTC_POWER_PIN, HIGH);
  delay(1000); // Wait for power to stabilize
}

bool ClockModule::initializeRTC()
{
  for (int i = 0; i < 3; i++)
  {
    if (rtc.begin())
    {
      return true;
    }
    powerCycleRTC();
    Serial.println("Couldn't find RTC, retrying...");
    delay(1000);
  }
  return false;
}

bool ClockModule::checkForZeroTime()
{
  DateTime currentTime = rtc.now();

  // Check if current time is 0:0:0
  bool isZeroTime = (currentTime.hour() == 0 && currentTime.minute() == 0 && currentTime.second() == 0);

  if (isZeroTime && wasZeroTime)
  {
    // Two consecutive zero time readings detected
    Serial.println("Two consecutive zero time readings detected, power cycling RTC");
    powerCycleRTC();
    if (!rtc.begin())
    {
      Serial.println("Failed to reinitialize RTC after power cycle");
      return false;
    }
    wasZeroTime = false;
    return true;
  }

  wasZeroTime = isZeroTime;
  lastTimeCheck = currentTime;
  return true;
}

bool ClockModule::isAlarmTriggered()
{
  if (!alarmTriggered)
  {
    return false;
  }

  alarmTriggered = false;
  resetAlarm();
  return true;
}

void ClockModule::resetAlarm()
{
  rtc.clearAlarm(1);
  Serial.println("Alarm detected and cleared.");
  if (!rtc.setAlarm1(
          DateTime(2024, 10, 1, 0, 0, 0),
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

uint8_t ClockModule::getHour()
{
  return rtc.now().hour();
}

uint8_t ClockModule::getMinute()
{
  return rtc.now().minute();
}

uint8_t ClockModule::getSecond()
{
  return rtc.now().second();
}

int ClockModule::getPercentOfDay(float hour)
{
  return (hour - hourStart) / (hourFinish - hourStart) * 100;
}

int ClockModule::getDayCompletionPercent()
{
  uint8_t currentHour = getHour();
  return getPercentOfDay(currentHour);
}

int ClockModule::getSafeExtensionPercent()
{
  return safeExtensionPercent;
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
  return (getHour() >= hourStart && getHour() <= hourFinish);
}

float ClockModule::getClockTemp()
{
  return rtc.getTemperature();
}

String ClockModule::getSimpleTimeString()
{
  String result = "";
  result.concat(getHour());
  result.concat(":");
  result.concat(getMinute());
  return result;
}

String ClockModule::getFullTimeString()
{
  if (!checkForZeroTime())
  {
    return "RTC Error";
  }
  String result = "";
  result.concat(getHour());
  result.concat(":");
  result.concat(getMinute());
  result.concat(":");
  result.concat(getSecond());
  return result;
}

uint32_t ClockModule::getTimestamp()
{
  return rtc.now().unixtime();
}

void ClockModule::setDateTime(const char *dateTimeString)
{
  rtc.adjust(DateTime(dateTimeString));
}

void ClockModule::setSimpleTime(int hour, int minute)
{
  uint16_t year = rtc.now().year();
  uint8_t month = rtc.now().month();
  uint8_t day = rtc.now().day();
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
  safeExtensionPercent = doc["setSafePercent"].as<float>();
  String output;
  serializeJson(doc, output);
  SettingsManager::upsertSetting("schedule", output);
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
  html.concat(R"%%(">
        <input type="submit" style="font-size:5vw;" value="Submit">      
      </form>
    </section>

    <section>
      <form action="/" method="POST">
        <input type="hidden" name="setTime" id="browserTime">
        <input type="submit" style="font-size:5vw;" value="Calibrate with Phone Time" onclick="document.getElementById('browserTime').value = new Date().toLocaleTimeString('en-US', { hour12: false, hour: '2-digit', minute: '2-digit' })">
      </form>
    </section>)%%");

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
        <br/>
        Set safe extension percent: 
        <input style="font-size:5vw;" type="number" max="100" min="0" name="setSafePercent" value=")");
  html.concat((int)safeExtensionPercent);
  html.concat(R"(">
        <input type="submit" style="font-size:5vw;" value="Submit">    
      </form>
    </section>
  )");
  return html;
}