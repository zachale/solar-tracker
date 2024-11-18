#ifndef ST_H
#define ST_H

#include "../Clock/Clock.h"
#include "../LinearActuator/LinearActuator.h"
#include "../WindSpeedSensor/WindSpeedSensor.h"
#include "../WifiClient/WifiClient.h"
#include "../ButtonPanel/ButtonPanel.h"
#include <Arduino.h>

class SolarTracker
{
public:
  enum Status
  {
    ACTIVE,
    NIGHT,
    SAFE,
    AWAY
  };
  // Not Ideal To Have These Public
  ClockModule clockModule;
  WindSpeedSensor windSensor;
  LinearActuator actuator;
  SolarTracker(void (*callback)());
  void setup();
  Status setStatus(Status);
  Status getStatus();
  String getStatusString(); 
  void pollSensorData();
  void sync();
  void syncClock();

private:
  Status status = ACTIVE;
  WifiClient wifiClient;
  static const String statusStrings[];
  unsigned long sensorTimer;
  static const int ACTUATOR_INTERRUPT_PIN = 2;
  static const int CLOCK_INTERRUPT_PIN = 3;
  const String timeAPIUrl = "https://worldtimeapi.org/api/timezone/America/Toronto";
  void extendActuatorOnHour();
  void extendActuatorToHalf();
  void updateStatus();
  void actOnStatus(Status);
};

#endif