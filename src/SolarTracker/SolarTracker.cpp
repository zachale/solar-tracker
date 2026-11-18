#include "./SolarTracker.h"
#include "../../secrets.h"
#include "../Watchdog/Watchdog.h"

const String SolarTracker::statusStrings[4] = {"ACTIVE", "NIGHT", "SAFE", "AWAY"};

extern "C" char *sbrk(int incr);

// The RAM between the top of the heap and the stack, which nothing has claimed yet. The
// tracker builds a lot of Strings, so a slow fall here over days is the heap fragmenting,
// visible in the log long before it runs out.
static int freeRam()
{
  char top;
  return &top - sbrk(0);
}

SolarTracker::SolarTracker(void (*callback)())
{
  // Initialize the linear actuator
  actuator.setExtensionCallBack(callback);
  otaUpdater.onBeforeRestart(handOffPosition, this);
}

// Right before an OTA restart: write down where the panel is, so the new firmware starts
// there instead of calibrating. OTA only installs with the actuator stopped.
void SolarTracker::handOffPosition(void *ctx, const char *version)
{
  SolarTracker *tracker = static_cast<SolarTracker *>(ctx);
  bool saved = PositionHandoff::save(version, tracker->actuator.snapshot(),
                                     tracker->clockModule.getTimestamp());
  Serial.println(saved ? "Position handed over to " + String(version)
                       : String("No position to hand over; the next boot calibrates"));
}

void SolarTracker::setup()
{
  attachInterrupt(digitalPinToInterrupt(ACTUATOR_INTERRUPT_PIN), actuator.countSteps, RISING);
  pinMode(CLOCK_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CLOCK_INTERRUPT_PIN), clockModule.setAlarmTriggered, FALLING);
  clockModule.setup();
  checkClockBus();
  windSensor.setup();
  actuator.calibrationAllowed = mayCalibrate();
  actuator.positionCalibrationAllowed = status != SAFE;
  LinearActuator::Snapshot handed;
  positionRestored = PositionHandoff::take(FIRMWARE_VERSION, clockModule.getTimestamp(), handed) &&
                     actuator.restore(handed);
  Serial.println(positionRestored ? "Position restored from the last firmware: " +
                                        String(actuator.getPercentExtended()) + "%, no calibration needed"
                                  : String("No position handed over; the first move calibrates"));
  dailySync();
  runBootCalibrationTest();
  initializeStatus();
}

// Timing builds only (BOOT_TEST_CALIBRATIONS > 0): a few calibrations in a row at every boot,
// so the legs they record (LinearActuator::legsQuery, sent with every sync) show how long a
// home and a max really take on this actuator. Never in away or SAFE; a wind that rises
// during a sweep ends the test after that sweep. initializeStatus() then moves to wherever
// the status wants the panel; at night it would stay where the last sweep left it, at full
// extension, so it goes to the end position instead.
void SolarTracker::runBootCalibrationTest()
{
  if (BOOT_TEST_CALIBRATIONS <= 0)
  {
    return;
  }
  for (int i = 0; i < BOOT_TEST_CALIBRATIONS && mayCalibrate(); i++)
  {
    Serial.println("Boot calibration test " + String(i + 1) + " of " + String(BOOT_TEST_CALIBRATIONS));
    actuator.recalibrate();
  }
  if (mayCalibrate() && statusForNow() == NIGHT)
  {
    actuator.extendToPercent(clockModule.getEndPercent());
  }
}

void SolarTracker::pollSensorData()
{
  actuator.calibrationAllowed = mayCalibrate();
  actuator.positionCalibrationAllowed = status != SAFE;

  if (status != AWAY && ButtonPanel::isAwayModeOverride())
  {
    setStatus(AWAY);
  }
  else if (status == AWAY && !ButtonPanel::isAwayModeOverride())
  {
    // Out of away: a calibration that came due while away runs now. In ACTIVE the move
    // does it first; at night nothing moves, so it runs here and goes back to where it was.
    // The status changes first: the sweep calls back into this loop.
    Status next = statusForNow();
    setStatus(next);
    if (next == NIGHT && actuator.calibrationDue() && actuator.status == LinearActuator::STOP)
    {
      Serial.println("Away ended with a calibration due");
      actuator.recalibrateInPlace(clockModule.getEndPercent());
    }
  }

#ifdef CLOCK_FAULT_WEDGE
  static unsigned long lastWedgeAt = 0;
  if (millis() - lastWedgeAt > 120000UL)
  {
    lastWedgeAt = millis();
    Serial.println(String("FAULT INJECTION: wedging the I2C bus: ") +
                   (clockModule.wedgeForTest() ? "the DS3231 is holding SDA low" : "could not wedge it"));
  }
#endif

  checkClockBus();

  if (clockModule.isAlarmTriggered())
  {
    // The alarm at the first hour past the schedule's end fires before the status block
    // turns the tracker to NIGHT, so the status alone would drive it past the last
    // scheduled position to full extension every evening.
    if (status == ACTIVE && clockModule.isActiveHours())
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
      windPeak = windSpeed;
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
    Serial.print("Free RAM: ");
    Serial.println(freeRam());
    sensorTimer = millis();
    updateStatus();
  }

  if (millis() - telemetryTimer > TELEMETRY_INTERVAL)
  {
    spoolTelemetryRow();
    telemetryTimer = millis();
  }

  // Only ever while the settings switch is off. With the switch on the radio is an
  // access point, and reaching the server would mean putting it into station mode,
  // which tears the access point down for good: the ESP32 stops handing out listening
  // sockets after four bring-ups in a power session.
  if (millis() - syncTimer > SYNC_INTERVAL_MS && !appMode)
  {
    syncTimer = millis();
    syncPending = true;
  }

  // The same rule as the sync: the radio is only ours with the settings switch off, out
  // of SAFE, and with the actuator stopped. Runs before serviceSync() so a download that
  // is in flight is seen as busy and the sync waits for it.
  otaUpdater.service(wifiClient, !appMode && status != SAFE && actuator.status == LinearActuator::STOP);

  serviceSync();
}

// Bus health first: everything in the loop reads the RTC, and a wedged bus returns register
// addresses and stack residue rather than failing loudly. Also runs from setup(), so a boot
// that finds the bus wedged is healed before the status is chosen from the time, and the
// chip's setup that the wedge made the clock skip is finished there.
void SolarTracker::checkClockBus()
{
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
    if (!back)
    {
      return;
    }
  }
  clockModule.completeSetup();
}

// A sync blocks the loop for as long as a WiFi association and a handful of POSTs
// take, which can be most of a minute. That is harmless when the tracker is idle
// and dangerous when it is not:
//
//   - moveTo() calls back into loop() on every iteration of its drive loop, with
//     the motor still energised. A sync started from inside that call would hold
//     the motor on for the whole request and drive the panel past its target.
//   - dailySync() can run the weekly calibration, and a full home-and-max sweep
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
  // The sync's WiFi.end() would kill an OTA download mid-transfer. It stays queued and
  // runs as soon as the download finishes or fails.
  if (otaUpdater.busy())
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
}

void SolarTracker::extendActuatorOnHour()
{
  int dayCompletePercentage = clockModule.getHourlyExtensionPercent();
  actuator.extendToPercent(dayCompletePercentage);
}

void SolarTracker::extendActuatorToSafe()
{
  int safePercentage = clockModule.getSafeExtensionPercent();
  actuator.extendToPercent(safePercentage);
}

SolarTracker::Status SolarTracker::setStatus(Status inputStatus)
{
  status = inputStatus;
  actuator.calibrationAllowed = mayCalibrate();
  actuator.positionCalibrationAllowed = status != SAFE;
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
  setStatus(statusForNow());
}

SolarTracker::Status SolarTracker::statusForNow()
{
  if (ButtonPanel::isAwayModeOverride())
  {
    return AWAY;
  }
  if (windSensor.status == WindSpeedSensor::HIGH_WIND)
  {
    return SAFE; // leaving away in a wind that is still up
  }
  return clockModule.isActiveHours() ? ACTIVE : NIGHT;
}

// The weekly calibration. Finding an unknown position is allowed more widely: see
// LinearActuator::positionCalibrationAllowed.
bool SolarTracker::mayCalibrate()
{
  // The switch as well as the status: at boot the status has not been chosen yet.
  return status != AWAY && status != SAFE && !ButtonPanel::isAwayModeOverride();
}

int SolarTracker::restingPercent()
{
  return status == ACTIVE ? clockModule.getHourlyExtensionPercent() : clockModule.getEndPercent();
}

void SolarTracker::updateStatus()
{
  if (status == AWAY)
  {
    // Away already holds the safe position. The wind sensor's own state still has to
    // follow the wind, or a gust while away leaves it stuck in HIGH_WIND: SAFE could never
    // trip again, and leaving away would not know the wind is up.
    if (!windSensor.enteringHighWind() && windSensor.status == WindSpeedSensor::HIGH_WIND)
    {
      windSensor.exitingHighWind();
    }
    return;
  }
  if (windSensor.enteringHighWind())
  {
    setStatus(SAFE);
  }
  else if (status == SAFE && windSensor.exitingHighWind())
  {
    // Not straight to ACTIVE: at night that would drive to the hour's position past the
    // schedule's end.
    setStatus(statusForNow());
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
  // The weekly calibration, on a known position. An unknown one is calibrated by the next
  // move that may. Away or in SAFE it keeps waiting, and runs on the first sync after.
  if (mayCalibrate() && actuator.getPercentExtended() >= 0 && actuator.calibrationDue())
  {
    Serial.println("Weekly calibration");
    actuator.recalibrateInPlace(restingPercent());
  }
  syncClock();
  uploadLogs();
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
    int code = wifiClient.post(API + String("/data"), body, "text/csv");
    Watchdog::progress();
    if (code != 200)
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
  // The firmware version rides along so the server's request log shows what each
  // tracker is running; the route ignores the query.
  // cal: calibrations since boot; pos: 1 when this boot took its position over from the
  // last firmware rather than calibrating. Both are for the request log; the route
  // ignores them.
  // The rest is LinearActuator::legsQuery(): the calibrated frame and the last sweep legs.
  String body = wifiClient.get(API + String("/time/est?fw=" FIRMWARE_VERSION "&cal=") +
                               String(LinearActuator::recalibrations) + "&pos=" + String(positionRestored ? 1 : 0) +
                               actuator.legsQuery());
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