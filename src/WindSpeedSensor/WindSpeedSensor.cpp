#include "./WindSpeedSensor.h"

bool WindSpeedSensor::isEnabled()
{
  if (ButtonPanel::isWindSensorEnabled())
  {
    return true;
  }
  status = DISABLED;
  return false;
}

int WindSpeedSensor::getSpeed()
{
  if (!isEnabled())
  {
    return 0;
  }
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

bool WindSpeedSensor::enteringHighWind()
{
  return status != HIGH_WIND && isMaintainingUpperSpeed();
}

bool WindSpeedSensor::isMaintainingLowerSpeed()
{
  if (getSpeed() < getLowerSpeedMax())
  {
    if (!lowerSpeedTimer)
    {
      lowerSpeedTimer = millis();
    }
    else if (millis() - lowerSpeedTimer > lowerSpeedDelay)
    {
      setStatus(ACTIVE);
      lowerSpeedTimer = 0;
      return true;
    }
  }
  return false;
}

bool WindSpeedSensor::exitingHighWind()
{
  return status != ACTIVE && isMaintainingLowerSpeed();
}

void WindSpeedSensor::setStatus(Status inputStatus)
{
  status = inputStatus;
}

String WindSpeedSensor::toHtml()
{
  String html;
  html.concat(R"(
    <h1 style="font-size:7vw;">Wind Sensor</h1>
    <section>
      <form action="/" method="POST">
        Set upper threshold wind speed max (km/h):
        <input style="font-size:5vw;" type="number" min="0" name="windUpperMaxSpeed" value=")");
  html.concat(String(getUpperSpeedMax() * 3.6)); // upper wind threshold speed m/s converted to km/h
  html.concat(R"(">
        <input type="submit" style="font-size:5vw;" value="Submit">
      </form>
    </section>
    <section>
      <form action="/" method="POST">
        Set upper threshold wait period (minutes):
        <input style="font-size:5vw;" type="number" min="0" name="windUpperWait" value=")");
  html.concat(String(getUpperSpeedDelay() / 6000)); // upper wind threshold wait period milliseconds converted to minutes
  html.concat(R"(">
        <input type="submit" style="font-size:5vw;" value="Submit">
      </form>
    </section>
    <section>
      <form action="/" method="POST">
        Set lower threshold wind speed max (km/h):
        <input style="font-size:5vw;" type="number" min="0" name="windLowerSpeedMax" value=")");
  html.concat(String(getLowerSpeedMax() * 3.6)); // lower wind threshold speed m/s converted to km/h
  html.concat(R"(">
        <input type="submit" style="font-size:5vw;" value="Submit">
      </form>
    </section>
    <section>
      <form action="/" method="POST">
        Set lower threshold wait period (minutes):
        <input style="font-size:5vw;" type="number" min="0" name="windLowerWait" value=")");
  html.concat(String(getLowerSpeedDelay() / 6000)); // lower wind threshold wait period milliseconds converted to minutes
  html.concat(R"(">
        <input type="submit" style="font-size:5vw;" value="Submit">
      </form>
    </section>
  )");
  return html;
}