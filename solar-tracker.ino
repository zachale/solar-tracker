// Date and time functions using a DS3231 RTC connected via I2C and Wire lib
#include "RTClib.h"
#include "./LinearActuator.h"
#include "./WindSensor.h"

RTC_DS3231 rtc;
LinearActuator actuator;
WindSensor windSensor;


float voltageConversionConstant = 0.004882814;
int sensorDelay = 1000;

unsigned long sensorTimer = 0;

void setup() {
  Serial.begin(57600);

#ifndef ESP8266
  while (!Serial)
    ;  // wait for serial port to connect. Needed for native USB
#endif

  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    Serial.flush();
    while (1) delay(10);
  }

  if (rtc.lostPower()) {
    Serial.println("RTC lost power, let's set the time!");
    // This line sets the RTC with an explicit date & time, for example to set
    // January 21, 2014 at 3am you would call:
    // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
  }
  attachInterrupt(digitalPinToInterrupt(2), actuator.countSteps, RISING);
  actuator.recalibrate();

  Serial.println("Setup complete.");

}

void loop() {
  if(millis() - sensorTimer > 1000){
    float windSpeed = windSensor.getWindSpeed();
    float temperature = rtc.getTemperature();
    uint8_t hour = rtc.now().hour();
    sensorTimer = millis();
  }

  actuator.extendToPercent(80);
  actuator.extendToPercent(10);
  actuator.extendToPercent(50);
  actuator.extendToPercent(100);
  
}


void extendOnTime(){

}

