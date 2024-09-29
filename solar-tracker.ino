#include "RTClib.h"
#include "./LinearActuator.h"
#include "./WindSensor.h"
#include "./Wifi.h"

RTC_DS3231 rtc;
LinearActuator actuator;
WindSensor windSensor;
WifiModule wifi(actuator, windSensor);


float voltageConversionConstant = 0.004882814;
int sensorDelay = 1000;

unsigned long sensorTimer = 0;

uint8_t hourStart = 8;
uint8_t hourFinish = 20;
uint8_t percentStart = 20;
uint8_t percentFinish = 80;

void setup() {
  Serial.begin(9600);

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
  Serial.println("Starting Setup");
  attachInterrupt(digitalPinToInterrupt(2), actuator.countSteps, RISING);
  wifi.setup();

  // actuator.recalibrate();

  Serial.println("Setup complete.");

}

void loop() {
  if(millis() - sensorTimer > 1000){
    float windSpeed = windSensor.getWindSpeed();
    // Serial.println(windSpeed);
    float temperature = rtc.getTemperature();
    uint8_t hour = rtc.now().hour();
    // Serial.println(rtc.now().minute());
    sensorTimer = millis();
  }
  wifi.checkForClient();

}

void compileCriticalSensorData(){
  float windSpeed = windSensor.getWindSpeed();
  float temperature = rtc.getTemperature();
  uint8_t time = rtc.now().unixtime();
  int percentExtended = actuator.getPercentExtended();
}

void extendActuatorOnHour(){
  uint8_t hour = rtc.now().hour();
  uint8_t dayCompletePercent = (hour - hourStart)/(hourFinish - hourStart) * 100;
  actuator.extendToPercent(normalizePercentage(dayCompletePercent));
}

int normalizePercentage(int percent){
  uint8_t difference = percentFinish - percentStart;
  return (difference * (percent/100)) + percentStart;
}

