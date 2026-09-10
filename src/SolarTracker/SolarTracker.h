#ifndef ST_H
#define ST_H

#include "../Clock/Clock.h"
#include "../LinearActuator/LinearActuator.h"
#include "../WindSpeedSensor/WindSpeedSensor.h"
#include "../WifiClient/WifiClient.h"
#include "../ButtonPanel/ButtonPanel.h"
#include "../SettingsManager/SettingsManager.h"
#include "../Telemetry/Telemetry.h"
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
  void dailySync();
  void syncClock();
  void uploadLogs();

private:
  Status status = ACTIVE;
  WifiClient wifiClient;
  static const String statusStrings[];
  unsigned long sensorTimer;
  unsigned long telemetryTimer = 0;
  // Highest wind speed seen since the last row was spooled. Ten second samples
  // are too coarse to average usefully, and a gust that triggers the safety
  // logic matters far more than the mean, so the peak is what gets recorded.
  int windPeak = 0;
  static const unsigned long TELEMETRY_INTERVAL = 600000;   // ten minutes
  static const int ACTUATOR_INTERRUPT_PIN = 2;
  static const int CLOCK_INTERRUPT_PIN = 3;
  void extendActuatorOnHour();
  void extendActuatorToSafe();
  void updateStatus();
  void actOnStatus(Status);
  void initializeStatus();
  void spoolTelemetryRow();
};

#endif