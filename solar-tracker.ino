#include "./Clock.h"
#include "./LinearActuator.h"
#include "./WindSpeedSensor.h"
#include "./Wifi.h"

ClockModule clockModule;
LinearActuator actuator(pollSensorData);
WindSpeedSensor windSensor;
WifiModule wifi(&actuator, &windSensor, &clockModule);

float voltageConversionConstant = 0.004882814;
int sensorDelay = 1000;

unsigned long sensorTimer = 0;

int status;
const int ACTIVE = 1;
const int NIGHT = 2;
const int SAFE = 3;
const int AWAY = 4;

bool alarmTriggered = false;

const int ACTUATOR_INTERRUPT_PIN = 2;

void setup() {

  Serial.begin(9600); //begining serial here because this is the first class initialized

#ifndef ESP8266
  while (!Serial)
    ;  // wait for serial port to connect. Needed for native USB
#endif
  
  clockModule.setup();
  wifi.setup();

  status = ACTIVE;
  Serial.println("Setup complete.");

}

void loop() {

  pollSensorData();

  if(clockModule.alarmTriggered()){
      if(clockModule.isActiveHours()){
        status = ACTIVE;
        extendActuatorOnHour();
      } else if (status == ACTIVE){
        extendActuatorToHalf();
        status = NIGHT;
      }
  }

  wifi.checkForClient();
}

void pollSensorData(){
  if(millis() - sensorTimer > 1000){
    // float windSpeed = windSensor.getSpeed();
    // Serial.print("Wind Speed: ");
    // Serial.print(windSpeed);
    // Serial.println();
    // float temperature = rtc.getTemperature();
    // Serial.print("Temperature: ");
    // Serial.print(temperature);
    // Serial.println();
    // int percentExtended = actuator.getPercentExtended();
    // Serial.print("PercentExtended: ");
    // Serial.print(percentExtended);
    // Serial.println();


    // Serial.print(now.year(), DEC);
    // Serial.print('/');
    // Serial.print(now.month(), DEC);
    // Serial.print('/');
    // Serial.print(now.day(), DEC);
    // Serial.print(now.hour(), DEC);
    // Serial.print(':');
    // Serial.print(now.minute(), DEC);
    // Serial.print(':');
    // Serial.print(now.second(), DEC);
    // Serial.println();
    // Serial.println(now.unixtime());

    if(status == ACTIVE && windSensor.highWindCheck()){
      extendActuatorToHalf();
      status = SAFE;
    } else if (status == SAFE && !windSensor.highWindCheck()){
      status = ACTIVE;
    }
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

