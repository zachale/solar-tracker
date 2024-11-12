#ifndef ST_H
#define ST_H

#include "../Clock/Clock.h"
#include "../LinearActuator/LinearActuator.h"
#include "../WindSpeedSensor/WindSpeedSensor.h"

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
  static const String statusStrings[];
  SolarTracker(void (*callback)());
  void setup();
  Status setStatus(Status);
  Status getStatus();
  void pollSensorData();

private:
  ClockModule clockModule;
  WindSpeedSensor windSensor;
  LinearActuator actuator;
  Status status;
  unsigned long sensorTimer;
  static const int ACTUATOR_INTERRUPT_PIN = 2;
  static const int CLOCK_INTERRUPT_PIN = 3;
  void extendActuatorOnHour();
  void extendActuatorToHalf();
  void updateStatus();
  void actOnStatus(Status);
};

#endif