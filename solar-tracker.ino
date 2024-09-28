// Date and time functions using a DS3231 RTC connected via I2C and Wire lib
#include "RTClib.h"
#include "./LinearActuator.h"


RTC_DS3231 rtc;
LinearActuator actuator;

const int windSensorPin = A3;
const int PWMBackward = 10;
const int PWMForward = 11;

float voltageConversionConstant = 0.004882814;
int sensorDelay = 1000;

float windVoltageMin = 0.4;
float windSpeedMin = 0;
float windVoltageMax = 2.0;
float windSpeedMax = 32.0;


int FORWARD = 1;
int BACKWARD = -1;

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
    // When time needs to be set on a new device, or after a power loss, the
    // following line sets the RTC to the date & time this sketch was compiled
    // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    // This line sets the RTC with an explicit date & time, for example to set
    // January 21, 2014 at 3am you would call:
    // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
  }

  pinMode(PWMBackward, OUTPUT);
  pinMode(PWMForward, OUTPUT);

  pinMode(2, INPUT);
  attachInterrupt(digitalPinToInterrupt(2), countStepsWrapper, RISING);
  Serial.println("Setup complete.");

  // When time needs to be re-set on a previously configured device, the
  // following line sets the RTC to the date & time this sketch was compiled
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  // This line sets the RTC with an explicit date & time, for example to set
  // January 21, 2014 at 3am you would call:
  // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));

    
}

void loop() {
  if(millis() - sensorTimer > 1000){
    float windSpeed = getWindSpeed();
    float temperature = getTemperature();
    uint8_t hour = getHour();
    sensorTimer = millis();
  }

  if(actuator.isHomed == 0){
    actuator.home();
  }
}

int getWindSpeed() {
  float sensorValue = analogRead(windSensorPin);
  float sensorVoltage = sensorValue * voltageConversionConstant;
  if (sensorVoltage <= windVoltageMin) {
    return 0;
  } else {

    return (sensorVoltage - windVoltageMin) * windSpeedMax / (windVoltageMax - windVoltageMin);
  }
}

float getTemperature() {
  return rtc.getTemperature();
}

uint8_t getHour() {
  DateTime now = rtc.now();
  return now.hour();
}

void countStepsWrapper() {
  actuator.countSteps();
}
