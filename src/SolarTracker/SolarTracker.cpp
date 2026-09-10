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

  if (clockModule.isAlarmTriggered())
  {
    if (clockModule.getHour() == 0)
    {
      syncDue = true;
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
  if (millis() - benchSyncTimer > BENCH_SYNC_INTERVAL)
  {
    syncDue = true;
    benchSyncTimer = millis();
  }
#endif
}

bool SolarTracker::isSyncDue()
{
  if (!syncDue)
  {
    return false;
  }
  syncDue = false;
  return true;
}

void SolarTracker::spoolTelemetryRow()
{
  Telemetry::append(Telemetry::formatRow(clockModule.getTimestamp(),
                                         clockModule.getClockTemp(),
                                         actuator.getPercentExtended(),
                                         getStatusString(),
                                         windPeak,
                                         ButtonPanel::settingsServerEnabled(),
                                         ButtonPanel::isWindSensorEnabled(),
                                         ButtonPanel::isAwayModeOverride()));
  windPeak = 0;
#if BENCH_TEST
  Serial.println("Spooled telemetry row, spool is now " + String(Telemetry::size()) + " bytes");
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

// The spool is only discarded on a confirmed 200, so a failed upload costs
// nothing beyond the delay until the next sync. setup() calls dailySync(), which
// means a power cycle also flushes whatever the card is holding.
void SolarTracker::uploadLogs()
{
  String body = Telemetry::readAll();
  if (body == "")
  {
    return;
  }
  Serial.println("Uploading " + String(body.length()) + " bytes of telemetry");
  if (wifiClient.post(API + String("/data"), body, "text/csv") == 200)
  {
    Telemetry::clear();
    Serial.println("Telemetry uploaded, spool cleared");
  }
  else
  {
    Serial.println("Telemetry upload failed, keeping spool");
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