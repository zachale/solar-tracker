#include "./WindSpeedSensor.h"

int WindSpeedSensor::getSpeed() {
  float sensorValue = analogRead(WIND_SENSOR_PIN);
  float sensorVoltage = sensorValue * voltageConversionConstant;
  if (sensorVoltage <= windVoltageMin) {
    return 0;
  } else {
    return (sensorVoltage - windVoltageMin) * windSpeedMax / (windVoltageMax - windVoltageMin);
  }
}

float WindSpeedSensor::getUpperSpeedMax() {
  return upperSpeedMax;
}

void WindSpeedSensor::setUpperSpeedMax(float inputSpeed) {
  upperSpeedMax = inputSpeed;
}

float WindSpeedSensor::getLowerSpeedMax(){
  return lowerSpeedMax;
}

void WindSpeedSensor::setLowerSpeedMax(float inputSpeed) {
  lowerSpeedMax = inputSpeed;
}

void WindSpeedSensor::setHighSpeedDelay(unsigned long inputDelay){
  highSpeedDelay = inputDelay;
}

unsigned long WindSpeedSensor::getHighSpeedDelay(){
  return highSpeedDelay;
}

void WindSpeedSensor::setLowSpeedDelay(unsigned long inputDelay){
  lowSpeedDelay = inputDelay;
}

unsigned long WindSpeedSensor::getLowSpeedDelay(){
  return lowSpeedDelay;
}

bool WindSpeedSensor::isMaintainingHighSpeed(){
  if(getSpeed() > getUpperSpeedMax()){
    if(!highSpeedTimer){
      setStatus(GUST_DETECTED);
      highSpeedTimer = millis(); 
    } else if(millis() - highSpeedTimer > highSpeedDelay){
      setStatus(HIGH_WIND);
      highSpeedTimer = 0;
      return true;
    }
  }
  return false;
}

bool WindSpeedSensor::isMaintainingLowSpeed(){
  if(getSpeed() < getLowerSpeedMax()){
    if(!lowSpeedTimer){
      lowSpeedTimer = millis(); 
    } else if (millis() - highSpeedTimer > lowSpeedDelay){
      setStatus(ACTIVE);
      lowSpeedTimer = 0;
      return true;
    }
  }
  return false;
}

// If windspeed is high for 1 minute -> flatten
// If windspeed is low for 15 minutes -> go back to normal
bool WindSpeedSensor::highWindCheck(){

  // if wind is high, start 1 minute timer
  // if wind stays high -> continue
  // if wind goes low -> cancel timer
  if(status != HIGH_WIND && isMaintainingHighSpeed()){
    return true;
  } else if (status == HIGH_WIND && isMaintainingLowSpeed()){
    return false;
  } 

  return false;
}

void WindSpeedSensor::setStatus(int inputStatus) {
  status = inputStatus;
}
