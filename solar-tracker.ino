#include "./src/Clock/Clock.h"
#include "./src/LinearActuator/LinearActuator.h"
#include "./src/WindSpeedSensor/WindSpeedSensor.h"
#include "./src/Wifi/Wifi.h"

int status = 0;
const int ACTIVE = 0;
const int NIGHT = 1;
const int SAFE = 2;
const int AWAY = 3;

ClockModule clockModule;
LinearActuator actuator(loop);
WindSpeedSensor windSensor;
WifiModule wifi(&status, &actuator, &windSensor, &clockModule);

int sensorDelay = 1000;

unsigned long sensorTimer = 0;
unsigned long statusTimer = 0;

const int ACTUATOR_INTERRUPT_PIN = 2;
const int CLOCK_INTERRUPT_PIN = 3;

void setup()
{

  Serial.begin(9600); // begining serial here because this is the first class initialized

#ifndef ESP8266
  while (!Serial)
    ; // wait for serial port to connect. Needed for native USB
#endif

  attachInterrupt(digitalPinToInterrupt(ACTUATOR_INTERRUPT_PIN), actuator.countSteps, RISING);

  pinMode(CLOCK_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CLOCK_INTERRUPT_PIN), clockModule.setAlarmTriggered, FALLING);

  clockModule.setup();
  wifi.setup();

  if (millis() - statusTimer > 1000)
  {
    updateStatus();
    statusTimer = millis();
  }

  updateStatus();
  Serial.println("Setup complete.");
}

void loop()
{
  pollSensorData();
  wifi.checkForClient();
  if (clockModule.isAlarmTriggered())
  {
    updateStatus();
  }
}

void pollSensorData()
{
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
    Serial.print("Status:");
    Serial.println(status);
    sensorTimer = millis();
  }
}

void extendActuatorOnHour()
{
  int dayCompletePercentage = clockModule.getHourlyExtensionPercent();
  actuator.extendToPercent(dayCompletePercentage);
}

void extendActuatorToHalf()
{
  int halfDayPercentage = clockModule.getHalfDayExtensionPercent();
  actuator.extendToPercent(halfDayPercentage);
}

void updateStatus()
{
  // TODO: Refactor this code to work in this new context
  // if(status == ACTIVE && windSensor.highWindCheck()){
  //   extendActuatorToHalf();
  //   status = SAFE;
  //   return;
  // } else if (status == SAFE && !windSensor.highWindCheck()){
  //   status = ACTIVE;
  // }

  if (clockModule.isActiveHours())
  {
    status = ACTIVE;
    Serial.println("Active hours");
    extendActuatorOnHour();
  }
  else if (status == ACTIVE)
  {
    status = NIGHT;
    Serial.println("Night Time");
    extendActuatorToHalf();
  }
}

void actOnStatus()
{
  if (status == ACTIVE)
  {
    Serial.println("Active hours");
    extendActuatorOnHour();
  }
  else if (status == AWAY)
  {
    Serial.println("Night Time");
    extendActuatorToHalf();
  }
  else if (status == SAFE)
  {
    Serial.println("Safe Mode");
    extendActuatorToHalf();
  }
}
