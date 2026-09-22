#include "./SolarTracker.h"
#include "../../secrets.h"

const String SolarTracker::statusStrings[4] = {"ACTIVE", "NIGHT", "SAFE", "AWAY"};

SolarTracker::SolarTracker(void (*callback)())
{
  // Initialize the linear actuator
  actuator.setExtensionCallBack(callback);
}

void SolarTracker::setup()
{
  attachInterrupt(digitalPinToInterrupt(ACTUATOR_INTERRUPT_PIN), actuator.countSteps, RISING);
  pinMode(CLOCK_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CLOCK_INTERRUPT_PIN), clockModule.setAlarmTriggered, FALLING);
  clockModule.setup();
  windSensor.setup();
  dailySync();
  initializeStatus();
}

void SolarTracker::pollSensorData()
{

  if (status != AWAY && ButtonPanel::isAwayModeOverride())
  {
    setStatus(AWAY);
  }
  else if (status == AWAY && !ButtonPanel::isAwayModeOverride())
  {
    setStatus(ACTIVE);
  }

  // Bus health first: everything below reads the RTC, and a wedged bus returns
  // register addresses and stack residue rather than failing loudly.
  if (!clockModule.isHealthy())
  {
    ClockModule::wedgeCount++;
    Serial.println("I2C bus unhealthy (code " + String(ClockModule::lastI2cCode) +
                   "), attempting bus clear #" + String(ClockModule::wedgeCount));
    bool freed = clockModule.busClear();
    bool back = clockModule.isHealthy();
    if (back)
    {
      ClockModule::recoverCount++;
    }
    Serial.println("Bus clear: sda_freed=" + String(freed ? 1 : 0) +
                   " recovered=" + String(back ? 1 : 0) +
                   " total_wedges=" + String(ClockModule::wedgeCount) +
                   " total_recovered=" + String(ClockModule::recoverCount));
  }

  if (clockModule.isAlarmTriggered())
  {
    // Only ever while the settings switch is off. With the switch on the radio is
    // an access point, and reaching the server would mean putting it into station
    // mode, which tears the access point down for good: the ESP32 stops handing
    // out listening sockets after four bring-ups in a power session.
    if (clockModule.getHour() % SYNC_EVERY_HOURS == 0 && !appMode)
    {
      syncPending = true;
    }
    if (status == ACTIVE)
    {
      extendActuatorOnHour();
    }
  }

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
    if (windSpeed > windPeak)
    {
      windPeak = (int)windSpeed;
    }
    int percentExtended = actuator.getPercentExtended();
    Serial.print("PercentExtended: ");
    Serial.print(percentExtended);
    Serial.println();
    Serial.println(clockModule.getFullTimeString());
    Serial.println(clockModule.getTimestamp());
    Serial.println(ButtonPanel::getButtonStatus());
    Serial.print("Status: ");
    Serial.println(getStatusString());
    sensorTimer = millis();
    updateStatus();
  }

  if (millis() - telemetryTimer > TELEMETRY_INTERVAL)
  {
    spoolTelemetryRow();
    telemetryTimer = millis();
  }

#if BENCH_TEST
  if (millis() - benchSyncTimer > BENCH_SYNC_INTERVAL && !appMode)
  {
    syncPending = true;
    benchSyncTimer = millis();
  }
#endif

  // Wall-clock sync fallback. Survives a wedged bus, so the unit keeps
  // reporting even while the clock is unreadable.
  if (millis() - monitorSyncTimer > MONITOR_SYNC_MS && !appMode)
  {
    monitorSyncTimer = millis();
    syncPending = true;
  }

  serviceSync();
}

// A sync blocks the loop for as long as a WiFi association and a handful of POSTs
// take, which can be most of a minute. That is harmless when the tracker is idle
// and dangerous when it is not:
//
//   - moveTo() calls back into loop() on every iteration of its drive loop, with
//     the motor still energised. A sync started from inside that call would hold
//     the motor on for the whole request and drive the panel past its target.
//   - dailySync() runs checkWeeklyRecalibration(), and a full home-and-max sweep
//     is the last thing wanted in the wind it was trying to get away from.
//
// So the sync waits for the actuator to stop and for the tracker to be out of SAFE.
// It is queued rather than dropped, so the window is delayed, never lost.
void SolarTracker::serviceSync()
{
  if (!syncPending || appMode)
  {
    return;
  }
  if (status == SAFE || actuator.status != LinearActuator::STOP)
  {
    return;
  }
  syncPending = false;
  dailySync();
}

void SolarTracker::spoolTelemetryRow()
{
  Telemetry::append(Telemetry::formatRow(clockModule.getTimestamp(),
                                         clockModule.getClockTemp(),
                                         actuator.getPercentExtended(),
                                         getStatusString(),
                                         ButtonPanel::isWindSensorEnabled()
                                             ? windPeak
                                             : (int)(ClockModule::wedgeCount > 99 ? 99 : ClockModule::wedgeCount),
                                         ButtonPanel::settingsServerEnabled(),
                                         ButtonPanel::isWindSensorEnabled(),
                                         ButtonPanel::isAwayModeOverride()));
  windPeak = 0;
#if BENCH_TEST
  Serial.println("Spooled row into chunk " + String(Telemetry::currentIndex()) + ", now " + String(Telemetry::chunkSize(Telemetry::currentIndex())) + " bytes");
#endif
}

void SolarTracker::extendActuatorOnHour()
{
  int dayCompletePercentage = clockModule.getHourlyExtensionPercent();
#if BENCH_TEST
  Serial.println("BENCH_TEST: would extend to " + String(dayCompletePercentage) + "%");
#else
  actuator.extendToPercent(dayCompletePercentage);
#endif
}

void SolarTracker::extendActuatorToSafe()
{
  int safePercentage = clockModule.getSafeExtensionPercent();
#if BENCH_TEST
  Serial.println("BENCH_TEST: would extend to safe " + String(safePercentage) + "%");
#else
  actuator.extendToPercent(safePercentage);
#endif
}

SolarTracker::Status SolarTracker::setStatus(Status inputStatus)
{
  status = inputStatus;
  actOnStatus(status);
  Serial.print("Set status to: ");
  Serial.println(getStatusString());
  return status;
}

SolarTracker::Status SolarTracker::getStatus()
{
  return status;
}

String SolarTracker::getStatusString()
{
  return statusStrings[getStatus()];
}

void SolarTracker::initializeStatus()
{
  if (clockModule.isActiveHours())
  {
    setStatus(ACTIVE);
  }
  else
  {
    setStatus(NIGHT);
  }
}

void SolarTracker::updateStatus()
{
  if (windSensor.enteringHighWind())
  {
    setStatus(SAFE);
  }
  else if (status == SAFE && windSensor.exitingHighWind())
  {
    setStatus(ACTIVE);
  }
  else if (status == NIGHT && clockModule.isActiveHours())
  {
    setStatus(ACTIVE);
  }
  else if (status == ACTIVE && !clockModule.isActiveHours())
  {
    setStatus(NIGHT);
  }
}

void SolarTracker::actOnStatus(Status inputStatus)
{
  if (inputStatus == ACTIVE)
  {
    extendActuatorOnHour();
  }
  else if (inputStatus == AWAY || inputStatus == SAFE)
  {
    extendActuatorToSafe();
  }
  else if (inputStatus == NIGHT)
  {
    // Do nothing, temporary addition
  }
}

void SolarTracker::setAppMode(bool isAppMode)
{
  appMode = isAppMode;
}

void SolarTracker::dailySync()
{
#if BENCH_TEST
  Serial.println("BENCH_TEST: skipping actuator recalibration");
#else
  actuator.checkWeeklyRecalibration();
#endif
  syncClock();
  uploadLogs();
  // REMOVED OTA UPDATER FOR NOW
}

// Uploads every chunk the card is holding, one request each, stopping at the first
// failure so nothing is lost and the order stays sane. A chunk is only deleted on
// a confirmed 200.
//
// Nothing is pushed while the settings switch is on. That is the whole point of
// the arrangement: in that mode the radio belongs to the access point and the
// tracker does not touch it.
void SolarTracker::uploadLogs()
{
  if (appMode)
  {
    Serial.println("App mode: holding telemetry on the card");
    return;
  }

  int uploaded = 0;
  for (int index = Telemetry::nextChunk(0); index >= 0; index = Telemetry::nextChunk(index + 1))
  {
    String body = Telemetry::readChunk(index);
    if (body == "")
    {
      continue;
    }

    Serial.println("Uploading chunk " + String(index) + ", " + String(body.length()) + " bytes");
    if (wifiClient.post(API + String("/data"), body, "text/csv") != 200)
    {
      Serial.println("Chunk " + String(index) + " failed, keeping it and stopping here");
      break;
    }

    Telemetry::clearChunk(index);
    uploaded++;
  }

  if (uploaded > 0)
  {
    Serial.println("Uploaded " + String(uploaded) + " chunk(s)");
  }

  // With the card empty, start the ring again from the beginning rather than
  // carrying on from wherever the write position had reached.
  if (Telemetry::nextChunk(0) < 0)
  {
    Telemetry::resume();
  }
}

void SolarTracker::syncClock()
{
  String body = wifiClient.get(API + String("/time/est"));
  if (body != "")
  {
    StaticJsonDocument<200> doc;
    deserializeJson(doc, body);
    const char *ISOdateTime = doc["datetime"];
    Serial.print("Syncing Clock to: ");
    Serial.println(ISOdateTime);
    clockModule.setDateTime(ISOdateTime);
  }
}