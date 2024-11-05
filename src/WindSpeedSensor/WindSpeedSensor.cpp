#include "./WindSpeedSensor.h"

int WindSpeedSensor::getSpeed()
{
  float sensorValue = analogRead(WIND_SENSOR_PIN);
  float sensorVoltage = sensorValue * voltageConversionConstant;
  if (sensorVoltage <= windVoltageMin)
  {
    return 0;
  }
  else
  {
    return (sensorVoltage - windVoltageMin) * windSpeedMax / (windVoltageMax - windVoltageMin);
  }
}

float WindSpeedSensor::getUpperSpeedMax()
{
  return upperSpeedMax;
}

void WindSpeedSensor::setUpperSpeedMax(float inputSpeed)
{
  Serial.println("Updated upper wind speed to");
  Serial.print(inputSpeed);
  Serial.print(" from ");
  Serial.print(upperSpeedMax);
  upperSpeedMax = inputSpeed;
}

float WindSpeedSensor::getLowerSpeedMax()
{
  return lowerSpeedMax;
}

void WindSpeedSensor::setLowerSpeedMax(float inputSpeed)
{
  Serial.println("Updated upper wind speed to");
  Serial.print(inputSpeed);
  Serial.print(" from ");
  Serial.print(lowerSpeedMax);
  lowerSpeedMax = inputSpeed;
}

void WindSpeedSensor::setUpperSpeedDelay(unsigned long inputDelay)
{
  Serial.println("Updated upper wind speed to");
  Serial.print(inputDelay);
  Serial.print(" from ");
  Serial.print(upperSpeedDelay);
  upperSpeedDelay = inputDelay;
}

unsigned long WindSpeedSensor::getUpperSpeedDelay()
{
  return upperSpeedDelay;
}

void WindSpeedSensor::setLowerSpeedDelay(unsigned long inputDelay)
{
  Serial.println("Updated upper wind speed to");
  Serial.print(inputDelay);
  Serial.print(" from ");
  Serial.print(lowerSpeedDelay);
  lowerSpeedDelay = inputDelay;
}

unsigned long WindSpeedSensor::getLowerSpeedDelay()
{
  return lowerSpeedDelay;
}

bool WindSpeedSensor::isMaintainingUpperSpeed()
{
  if (getSpeed() > getUpperSpeedMax())
  {
    if (!upperSpeedTimer)
    {
      setStatus(GUST_DETECTED);
      upperSpeedTimer = millis();
    }
    else if (millis() - upperSpeedTimer > upperSpeedDelay)
    {
      setStatus(HIGH_WIND);
      upperSpeedTimer = 0;
      return true;
    }
  }
  return false;
}

bool WindSpeedSensor::isMaintainingLowerSpeed()
{
  if (getSpeed() < getLowerSpeedMax())
  {
    if (!lowerSpeedTimer)
    {
      lowerSpeedTimer = millis();
    }
    else if (millis() - upperSpeedTimer > lowerSpeedDelay)
    {
      setStatus(ACTIVE);
      lowerSpeedTimer = 0;
      return true;
    }
  }
  return false;
}

// If windspeed is high for 1 minute -> flatten
// If windspeed is low for 15 minutes -> go back to normal
bool WindSpeedSensor::highWindCheck()
{

  // if wind is high, start 1 minute timer
  // if wind stays high -> continue
  // if wind goes low -> cancel timer
  if (status != HIGH_WIND && isMaintainingUpperSpeed())
  {
    return true;
  }
  else if (status == HIGH_WIND && isMaintainingLowerSpeed())
  {
    return false;
  }

  return false;
}

void WindSpeedSensor::setStatus(int inputStatus)
{
  status = inputStatus;
}
