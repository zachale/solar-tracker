#include "./WindSensor.h"

int WindSensor::getWindSpeed() {
  float sensorValue = analogRead(windSensorPin);
  float sensorVoltage = sensorValue * voltageConversionConstant;
  if (sensorVoltage <= windVoltageMin) {
    return 0;
  } else {
    return (sensorVoltage - windVoltageMin) * windSpeedMax / (windVoltageMax - windVoltageMin);
  }
}

float WindSensor::getWindSpeedMax() {
  return windSpeedMax;
}

void WindSensor::setWindSpeedMax(float inputSpeed) {
  windSpeedMax = inputSpeed;
}
