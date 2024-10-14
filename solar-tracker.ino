#include "./Clock.h"
#include "./LinearActuator.h"
#include "./WindSpeedSensor.h"
#include "./Wifi.h"

int status = 0;
const int ACTIVE = 0;
const int NIGHT = 1;
const int SAFE = 2;
const int AWAY = 3;

ClockModule clockModule;
LinearActuator actuator(loop);
WindSpeedSensor windSensor;
WifiModule wifi(&status, &actuator, &windSensor, &clockModule);

float voltageConversionConstant = 0.004882814;
int sensorDelay = 1000;

unsigned long sensorTimer = 0;
unsigned long statusTimer = 0;



const int ACTUATOR_INTERRUPT_PIN = 2;
const int CLOCK_INTERRUPT_PIN = 3;


void setup() {

  Serial.begin(9600); //begining serial here because this is the first class initialized

#ifndef ESP8266
  while (!Serial)
    ;  // wait for serial port to connect. Needed for native USB
#endif
  
  attachInterrupt(digitalPinToInterrupt(ACTUATOR_INTERRUPT_PIN), actuator.countSteps, RISING);

  pinMode(CLOCK_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CLOCK_INTERRUPT_PIN), clockModule.setAlarmTriggered, FALLING);

  clockModule.setup();
  wifi.setup();

  if(millis() - statusTimer > 1000){
    checkStatus();
    statusTimer = millis();
  }

  status = ACTIVE;
  Serial.println("Setup complete.");
  checkStatus();
}

void loop() {
  pollSensorData();
  wifi.checkForClient();
  if(clockModule.isAlarmTriggered()){
    checkStatus();
  }
}

void pollSensorData(){
  if(millis() - sensorTimer > 10000){
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
    sensorTimer = millis();
  }
}

void extendActuatorOnHour(){
  int dayCompletePercentage = clockModule.getHourlyExtensionPercent();
  actuator.extendToPercent(dayCompletePercentage);
}

void extendActuatorToHalf(){
  int halfDayPercentage = clockModule.getHalfDayExtensionPercent();
  actuator.extendToPercent(halfDayPercentage);
}

void checkStatus(){
  // TODO: Refactor this code to work in this new context
  // if(status == ACTIVE && windSensor.highWindCheck()){
  //   extendActuatorToHalf();
  //   status = SAFE;
  //   return;
  // } else if (status == SAFE && !windSensor.highWindCheck()){
  //   status = ACTIVE;
  // }

      if(clockModule.isActiveHours()){
        status = ACTIVE;
        Serial.println("Active hours");
        extendActuatorOnHour();
      } else if (status == ACTIVE){
        Serial.println("Night Time");
        extendActuatorToHalf();
        status = NIGHT;
      }
  
}

