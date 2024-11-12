#include "./SolarTracker.h"

const String SolarTracker::statusStrings[] = {"Active", "Night Mode", "Safe Mode", "Away Mode"};

SolarTracker::SolarTracker(void (*callback)())
{
  // Initialize the linear actuator
  actuator.setExtensionCallBack(callback);
  // Initialize the status
  setStatus(ACTIVE);
}

void SolarTracker::setup()
{
  attachInterrupt(digitalPinToInterrupt(ACTUATOR_INTERRUPT_PIN), actuator.countSteps, RISING);
  pinMode(CLOCK_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CLOCK_INTERRUPT_PIN), clockModule.setAlarmTriggered, FALLING);
  clockModule.setup();
}

void SolarTracker::pollSensorData()
{
  if (millis() - sensorTimer > 10000)
  {
    updateStatus();
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
    Serial.print("Status:");
    Serial.println(status);
    sensorTimer = millis();
  }
}

void SolarTracker::extendActuatorOnHour()
{
  if (clockModule.isAlarmTriggered())
  {
    int dayCompletePercentage = clockModule.getHourlyExtensionPercent();
    actuator.extendToPercent(dayCompletePercentage);
  }
}

void SolarTracker::extendActuatorToHalf()
{
  if (actuator.status != LinearActuator::HALF)
  {
    int halfDayPercentage = clockModule.getHalfDayExtensionPercent();
    actuator.extendToPercent(halfDayPercentage);
  }
}

SolarTracker::Status SolarTracker::setStatus(Status inputStatus)
{
  status = inputStatus;
  actOnStatus(status);
  return status;
}

SolarTracker::Status SolarTracker::getStatus()
{
  return status;
}

void SolarTracker::updateStatus()
{
  if (status == ACTIVE && windSensor.enteringHighWind())
  {
    setStatus(SAFE);
  }
  else if (status == SAFE && windSensor.exitingHighWind())
  {
    setStatus(ACTIVE);
  }
  else if (clockModule.isActiveHours())
  {
    setStatus(ACTIVE);
  }
  else if (status == ACTIVE)
  {
    setStatus(NIGHT);
  }
  else if (status == NIGHT && clockModule.isActiveHours())
  {
    setStatus(ACTIVE);
  }
  else if (status == ACTIVE && !clockModule.isActiveHours())
  {
    setStatus(NIGHT);
  }
  else
  {
    setStatus(AWAY);
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
    // Do nothing, stay extended, temporary addition
  }
}