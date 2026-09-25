#ifndef ST_H
#define ST_H

#include "../Clock/Clock.h"
#include "../LinearActuator/LinearActuator.h"
#include "../WindSpeedSensor/WindSpeedSensor.h"
#include "../WifiClient/WifiClient.h"
#include "../ButtonPanel/ButtonPanel.h"
#include "../SettingsManager/SettingsManager.h"
#include "../Telemetry/Telemetry.h"
#include "../OtaUpdater/OtaUpdater.h"
#include "../BenchTest.h"
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
  // Latched at boot by the sketch. Decides whether this power session is allowed
  // to use the radio as a station at all.
  void setAppMode(bool);
  // True while an OTA download is in flight. Anything that would reset the radio waits.
  bool otaBusy() const { return otaUpdater.busy(); }
  // Over-the-air trial bookkeeping; first thing in setup() after the watchdog.
  void otaBoot(uint8_t sdChipSelect) { otaUpdater.boot(sdChipSelect); }

private:
  Status status = ACTIVE;
  WifiClient wifiClient;
  OtaUpdater otaUpdater;
  static const String statusStrings[];
  unsigned long sensorTimer;
  unsigned long telemetryTimer = 0;
  // Highest wind speed seen since the last row was spooled. Ten second samples
  // are too coarse to average usefully, and a gust that triggers the safety
  // logic matters far more than the mean, so the peak is what gets recorded.
  int windPeak = 0;
  bool appMode = false;
  // A sync is queued rather than run the moment its hour arrives, because the hour
  // can arrive while the actuator is driving. See serviceSync().
  bool syncPending = false;
  // Syncs run on millis(), not on the DS3231 alarm. Re-arming the alarm needs the I2C
  // bus, so a wedged bus would silence the unit exactly when we most want to hear
  // from it. Only with the settings switch off, where a sync costs nothing but a
  // station association: a batch every fifteen minutes keeps each body small and
  // shows a fault in the cloud within the quarter hour.
  unsigned long syncTimer = 0;
#if BENCH_TEST
  static const unsigned long TELEMETRY_INTERVAL = 15000;    // fifteen seconds
  static const unsigned long SYNC_INTERVAL_MS = 120000;     // two minutes
#else
  static const unsigned long TELEMETRY_INTERVAL = 300000;   // five minutes, three rows a batch
  static const unsigned long SYNC_INTERVAL_MS = 900000;     // fifteen minutes
#endif
  static const int ACTUATOR_INTERRUPT_PIN = 2;
  static const int CLOCK_INTERRUPT_PIN = 3;
  void extendActuatorOnHour();
  void extendActuatorToSafe();
  void updateStatus();
  void actOnStatus(Status);
  void initializeStatus();
  void spoolTelemetryRow();
  void serviceSync();
};

#endif