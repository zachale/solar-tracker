// Date and time functions using a DS3231 RTC connected via I2C and Wire lib
#include "RTClib.h"

RTC_DS3231 rtc;

const int windSensorPin = A3;

float voltageConversionConstant = 0.004882814;
int sensorDelay = 1000;

float windVoltageMin = 0.4;
float windSpeedMin = 0;
float windVoltageMax = 2.0;
float windSpeedMax = 32.0;

void setup () {
  Serial.begin(57600);

#ifndef ESP8266
  while (!Serial); // wait for serial port to connect. Needed for native USB
#endif

  if (! rtc.begin()) {
    Serial.println("Couldn't find RTC");
    Serial.flush();
    while (1) delay(10);
  }

  if (rtc.lostPower()) {
    Serial.println("RTC lost power, let's set the time!");
    // When time needs to be set on a new device, or after a power loss, the
    // following line sets the RTC to the date & time this sketch was compiled
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    // This line sets the RTC with an explicit date & time, for example to set
    // January 21, 2014 at 3am you would call:
    // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
  }

  // When time needs to be re-set on a previously configured device, the
  // following line sets the RTC to the date & time this sketch was compiled
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  // This line sets the RTC with an explicit date & time, for example to set
  // January 21, 2014 at 3am you would call:
  // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
}

void loop () {
    
    float windSpeed = getWindSpeed();
    float temperature = getTemperature();
    uint8_t hour = getHour();
    Serial.println();
    Serial.println(windSpeed);
    Serial.println(temperature);
    Serial.println(hour);
    delay(sensorDelay);
}

float getWindSpeed(){
  float sensorValue = analogRead(windSensorPin);
  float sensorVoltage = sensorValue * voltageConversionConstant;

  Serial.println(sensorVoltage);

  if(sensorVoltage <= windVoltageMin){
    return 0;
  } else {
    return (sensorVoltage-windVoltageMin)*windSpeedMax/(windVoltageMax-windVoltageMin);
  }
}

float getTemperature() {
  return rtc.getTemperature();
}

uint8_t getHour(){
  DateTime now = rtc.now();
  return now.hour();
}

bool driveLinearActuator(int windSpeed){
  
}
