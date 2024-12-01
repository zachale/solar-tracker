#include "./SolarTracker.h"
#include "../../secrets.h"

const String SolarTracker::statusStrings[4] = {"ACTIVE", "NIGHT", "SAFE", "AWAY"};

SolarTracker::SolarTracker(void (*callback)())
{
  // Initialize the linear actuator
  actuator.setExtensionCallBack(callback);
}

void SolarTracker::setup()
{
  attachInterrupt(digitalPinToInterrupt(ACTUATOR_INTERRUPT_PIN), actuator.countSteps, RISING);
  pinMode(CLOCK_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CLOCK_INTERRUPT_PIN), clockModule.setAlarmTriggered, FALLING);
  clockModule.setup();
  dailySync();
  initializeStatus();
}

void SolarTracker::pollSensorData()
{
  if (clockModule.isAlarmTriggered() && status == ACTIVE)
  {
    if (clockModule.getHour() == 8)
    {
      dailySync();
    }
    extendActuatorOnHour();
  }

  if (millis() - sensorTimer > 10000)
  {
    float windSpeed = windSensor.getSpeed();
    Serial.print("Wind Speed: ");
    Serial.print(windSpeed);
    Serial.println();
    float temperature = clockModule.getClockTemp();
    Serial.print("Temperature: ");
    Serial.print(temperature);
    Serial.println();
    int percentExtended = actuator.getPercentExtended();
    Serial.print("PercentExtended: ");
    Serial.print(percentExtended);
    Serial.println();
    Serial.println(clockModule.getFullTimeString());
    Serial.println(clockModule.getTimestamp());
    Serial.print("Status: ");
    Serial.println(getStatusString());
    sensorTimer = millis();
    updateStatus();
  }
}

void SolarTracker::extendActuatorOnHour()
{
  int dayCompletePercentage = clockModule.getHourlyExtensionPercent();
  actuator.extendToPercent(dayCompletePercentage);
}

void SolarTracker::extendActuatorToHalf()
{
  int halfDayPercentage = clockModule.getHalfDayExtensionPercent();
  actuator.extendToPercent(halfDayPercentage);
}

SolarTracker::Status SolarTracker::setStatus(Status inputStatus)
{
  status = inputStatus;
  actOnStatus(status);
  Serial.print("Set status to: ");
  Serial.println(getStatusString());
  return status;
}

SolarTracker::Status SolarTracker::getStatus()
{
  return status;
}

String SolarTracker::getStatusString()
{
  return statusStrings[getStatus()];
}

void SolarTracker::initializeStatus()
{
  if (clockModule.isActiveHours())
  {
    setStatus(ACTIVE);
  }
  else
  {
    setStatus(NIGHT);
  }
}

void SolarTracker::updateStatus()
{
  if (windSensor.enteringHighWind())
  {
    setStatus(SAFE);
  }
  else if (status == SAFE && windSensor.exitingHighWind())
  {
    setStatus(ACTIVE);
  }
  else if (status == NIGHT && clockModule.isActiveHours())
  {
    setStatus(ACTIVE);
  }
  else if (status == ACTIVE && !clockModule.isActiveHours())
  {
    setStatus(NIGHT);
  }
}

void SolarTracker::actOnStatus(Status inputStatus)
{
  if (inputStatus == ACTIVE)
  {
    extendActuatorOnHour();
  }
  else if (inputStatus == AWAY || inputStatus == SAFE)
  {
    extendActuatorToHalf();
  }
  else if (inputStatus == NIGHT)
  {
    // Do nothing, temporary addition
  }
}

void SolarTracker::dailySync()
{
  if (!ButtonPanel::settingsServerEnabled())
  {
    syncClock();
    if (ota.updateAvailable())
    {
      ota.update();
    }
  }
}

void SolarTracker::syncClock()
{
  String body = wifiClient.get(API + String("/time/est"));
  if (body != "")
  {
    StaticJsonDocument<200> doc;
    deserializeJson(doc, body);
    const char *ISOdateTime = doc["datetime"];
    Serial.print("Syncing Clock to: ");
    Serial.println(ISOdateTime);
    clockModule.setDateTime(ISOdateTime);
  }
}