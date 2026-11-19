// Tests for SolarTracker's decisions about where the panel goes: after the dashboard's
// Recalibrate, and when the dashboard saves a new schedule.
//
// The real src/SolarTracker/SolarTracker.cpp runs here. The radio, OTA and watchdog behind
// it are no-ops (test/stubs/tracker_deps.cpp), and the actuator drives the bench motor below.
#define private public
#define protected public
#include "../src/SolarTracker/SolarTracker.h"
#undef private
#undef protected

#include "doctest.h"

namespace {
// The bench motor from the actuator tests, wired to a tracker: one or two hall pulses per
// loop pass while it is driven and not against an end stop, 20 ms a pass, 1000 pulses of
// stroke. `simHook` runs once a pass, standing in for the rest of the sketch's loop.
SolarTracker *simTracker = nullptr;
long simPos = 0;
const long SIM_LIMIT = 1000;
long simPass = 0;
long simDriven = 0; // passes with the motor driven
void (*simHook)() = nullptr;

void simMotor() {
  mock::advance(20);
  simPass++;
  LinearActuator &a = simTracker->actuator;
  bool driven = a.status != LinearActuator::STOP;
  if (driven) simDriven++;
  int pulses = (simPass % 3 == 0) ? 2 : 1;
  for (int i = 0; i < pulses; i++) {
    bool moves = (driven && a.dir > 0 && simPos < SIM_LIMIT) || (driven && a.dir < 0 && simPos > 0);
    if (moves) { simPos += a.dir > 0 ? 1 : -1; mock::now_us += 600; LinearActuator::countSteps(); }
  }
  if (simHook) simHook();
}

// A schedule as the dashboard's form sends it: every value a string.
void schedule(SolarTracker &t, int startHour, int startPercent, int middleHour, int middlePercent,
              int endHour, int endPercent, int safePercent) {
  JsonDocument doc;
  doc["setStartHour"] = std::to_string(startHour);
  doc["setStartPercent"] = std::to_string(startPercent);
  doc["setMiddleHour"] = std::to_string(middleHour);
  doc["setMiddlePercent"] = std::to_string(middlePercent);
  doc["setEndHour"] = std::to_string(endHour);
  doc["setEndPercent"] = std::to_string(endPercent);
  doc["setSafePercent"] = std::to_string(safePercent);
  t.clockModule.setSchedule(doc);
}

// A calibrated tracker at `hour`:`minute` in `status`, on a 6-to-16 schedule (10%, 50% at
// noon, 95% at the end, 80% safe), with the panel stopped at `atPercent` and that as its target.
SolarTracker &rig(SolarTracker &t, SolarTracker::Status status, int hour, int minute, int atPercent) {
  mock::reset();
  mock::advance(10000);
  simTracker = &t;
  simHook = nullptr;
  simPass = 0;
  simDriven = 0;
  LinearActuator::steps = 0;
  LinearActuator::recalibrations = 0;
  LinearActuator::lastRecalibrationTime = millis();
  t.clockModule.rtc.mockNow = DateTime(2026, 11, 18, (uint8_t)hour, (uint8_t)minute, 0);
  schedule(t, 6, 10, 12, 50, 16, 95, 80);
  t.status = status;
  t.actuator.calibrationAllowed = t.mayCalibrate();
  t.actuator.positionCalibrationAllowed = status != SolarTracker::SAFE;
  t.actuator.calibrationRequested = false;
  t.actuator.isHomed = 0;
  t.actuator.maxPos = SIM_LIMIT;
  t.actuator.pos = simPos = atPercent * SIM_LIMIT / 100;
  t.actuator.currentPercentTarget = atPercent;
  return t;
}

// Stopped within a couple of pulses of `percent`, with that as the target.
void checkParkedAt(SolarTracker &t, int percent) {
  CAPTURE(simPos);
  bool stopped = t.actuator.status == LinearActuator::STOP;
  CHECK(stopped);
  CHECK(t.actuator.currentPercentTarget == percent);
  CHECK(labs(simPos - percent * SIM_LIMIT / 100) <= 2);
}
} // namespace

TEST_SUITE("tracker: the dashboard's Recalibrate") {

  TEST_CASE("ends at the hour's position, not at full extension") {
    // The field, 2026-11-18: the 9:00 move had not happened, the panel was still at the 8:00
    // position, and Recalibrate left it at 100% until he power-cycled it. It goes where the
    // schedule says now, not back to the stale spot it started from.
    SolarTracker t(simMotor);
    rig(t, SolarTracker::ACTIVE, 9, 28, 23);
    int hour = t.clockModule.getHourlyExtensionPercent();
    REQUIRE(hour != 23);

    t.recalibrateAndReturn();

    CHECK(LinearActuator::recalibrations == 1);
    CHECK(t.actuator.maxPos == SIM_LIMIT);
    checkParkedAt(t, hour);
  }

  TEST_CASE("at night, ends at the schedule's end position") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::NIGHT, 21, 0, 60);

    t.recalibrateAndReturn();

    CHECK(LinearActuator::recalibrations == 1);
    checkParkedAt(t, 95);
  }

  TEST_CASE("in SAFE, ends at the safe position") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::SAFE, 11, 0, 30);

    t.recalibrateAndReturn();

    CHECK(LinearActuator::recalibrations == 1);
    checkParkedAt(t, 80);
  }

  TEST_CASE("in away, ends at the safe position") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::AWAY, 11, 0, 30);

    t.recalibrateAndReturn();

    CHECK(LinearActuator::recalibrations == 1);
    checkParkedAt(t, 80);
  }

  TEST_CASE("still ACTIVE in the seconds after the schedule's end, ends at the end position") {
    // ACTIVE turns to NIGHT on the 10 s status tick, so a sweep can end just past the last
    // hour with the status not yet changed. The hour's position there lies beyond the end.
    SolarTracker t(simMotor);
    rig(t, SolarTracker::ACTIVE, 17, 0, 60);
    REQUIRE_FALSE(t.clockModule.isActiveHours());

    t.recalibrateAndReturn();

    checkParkedAt(t, 95);
  }

  TEST_CASE("with the tracker's own loop running, a sweep across the end of the day ends there") {
    // The whole poll runs as the sketch's loop does, nested in the sweep: the clock passes
    // 17:00 near the end of the max leg, and the status tick turns NIGHT a few seconds later.
    SolarTracker t(simMotor);
    rig(t, SolarTracker::ACTIVE, 16, 56, 95);
    t.sensorTimer = t.telemetryTimer = t.syncTimer = millis();
    static bool past = false;
    past = false;
    simHook = [] {
      if (!past && simTracker->actuator.status == LinearActuator::MAXING && simPos >= 950) {
        simTracker->clockModule.rtc.mockNow = DateTime(2026, 11, 18, 17, 0, 1);
        past = true;
      }
      simTracker->pollSensorData();
    };

    t.recalibrateAndReturn();
    for (int i = 0; i < 3000; i++) simMotor(); // a minute of the loop afterwards

    simHook = nullptr;
    REQUIRE(past);
    CHECK(t.status == SolarTracker::NIGHT);
    checkParkedAt(t, 95);
  }

  TEST_CASE("a request made during the sweep is where it ends") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::ACTIVE, 9, 28, 30);
    simHook = [] {
      if (simPass == 100) simTracker->actuator.extendToPercent(45); // the dashboard, mid-sweep
    };

    t.recalibrateAndReturn();

    CHECK(LinearActuator::recalibrations == 1);
    checkParkedAt(t, 45);
  }

  TEST_CASE("pressed during a move, the move finishes and the calibration is owed") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::ACTIVE, 9, 28, 0);
    simHook = [] {
      if (simPass == 100) simTracker->recalibrateAndReturn();
    };

    t.actuator.extendToPercent(60);

    CHECK(LinearActuator::recalibrations == 0);
    CHECK(t.actuator.calibrationRequested);
    checkParkedAt(t, 60);
  }
}

TEST_SUITE("tracker: saving a schedule") {

  TEST_CASE("moves to the new schedule's position for this hour right away") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::ACTIVE, 8, 10, 23);
    REQUIRE(t.clockModule.getHourlyExtensionPercent() == 23);

    schedule(t, 6, 20, 12, 66, 18, 95, 80);
    int hour = t.clockModule.getHourlyExtensionPercent();
    REQUIRE(hour != 23);
    t.applySchedule();

    CHECK(t.status == SolarTracker::ACTIVE);
    checkParkedAt(t, hour);
  }

  TEST_CASE("a schedule that has already ended today turns to NIGHT at the new end position") {
    // Without the status change this would interpolate past the schedule's end and drive to
    // full extension, then sit there all night.
    SolarTracker t(simMotor);
    rig(t, SolarTracker::ACTIVE, 15, 30, 84);

    schedule(t, 6, 10, 12, 50, 14, 90, 80);
    t.applySchedule();

    CHECK(t.status == SolarTracker::NIGHT);
    checkParkedAt(t, 90);
  }

  TEST_CASE("a schedule that has already started today turns to ACTIVE at the hour's position") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::NIGHT, 7, 30, 95);
    schedule(t, 8, 10, 12, 50, 16, 95, 80);
    REQUIRE_FALSE(t.clockModule.isActiveHours());

    schedule(t, 6, 10, 12, 50, 16, 95, 80);
    int hour = t.clockModule.getHourlyExtensionPercent();
    t.applySchedule();

    CHECK(t.status == SolarTracker::ACTIVE);
    checkParkedAt(t, hour);
  }

  TEST_CASE("in SAFE, stays SAFE and goes to the new safe position") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::SAFE, 15, 30, 80);

    schedule(t, 6, 10, 12, 50, 14, 95, 70); // also ends the day before now
    t.applySchedule();

    CHECK(t.status == SolarTracker::SAFE);
    checkParkedAt(t, 70);
  }

  TEST_CASE("in away, stays away at the safe position") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::AWAY, 9, 0, 80);

    schedule(t, 6, 20, 12, 66, 18, 95, 80);
    t.applySchedule();

    CHECK(t.status == SolarTracker::AWAY);
    CHECK(simDriven == 0);
    checkParkedAt(t, 80);
  }

  TEST_CASE("saving the same schedule does not move the panel") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::ACTIVE, 9, 0, 30);
    REQUIRE(t.clockModule.getHourlyExtensionPercent() == 30);

    schedule(t, 6, 10, 12, 50, 16, 95, 80);
    t.applySchedule();

    CHECK(simDriven == 0);
    checkParkedAt(t, 30);
  }

  TEST_CASE("in the schedule's last hour, ACTIVE goes to the new end position") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::ACTIVE, 16, 30, 95);

    schedule(t, 6, 10, 12, 50, 16, 90, 80);
    t.applySchedule();

    CHECK(t.status == SolarTracker::ACTIVE);
    checkParkedAt(t, 90);
  }

  TEST_CASE("in SAFE with the position unknown, holds still") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::SAFE, 11, 0, 50);
    t.actuator.maxPos = 0;
    t.actuator.currentPercentTarget = -1;

    schedule(t, 6, 10, 12, 50, 16, 95, 70);
    t.applySchedule();

    CHECK(simDriven == 0);
    CHECK(LinearActuator::recalibrations == 0);
  }

  TEST_CASE("in away with the position unknown, finds it and goes to the new safe position") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::AWAY, 11, 0, 50);
    t.actuator.maxPos = 0;
    t.actuator.currentPercentTarget = -1;

    schedule(t, 6, 10, 12, 50, 16, 95, 70);
    t.applySchedule();

    CHECK(LinearActuator::recalibrations == 1);
    checkParkedAt(t, 70);
  }

  TEST_CASE("saved during a calibration sweep, the move goes to the new position") {
    // Even when the new position is the one held before the sweep began.
    SolarTracker t(simMotor);
    rig(t, SolarTracker::ACTIVE, 9, 0, 23);
    t.actuator.calibrationRequested = true;
    REQUIRE(t.clockModule.getHourlyExtensionPercent() == 30);
    simHook = [] {
      if (simPass == 100) {
        schedule(*simTracker, 7, 5, 12, 50, 16, 95, 80); // 9:00 on this one is 23
        simTracker->applySchedule();
      }
    };

    t.actuator.extendToPercent(30); // the 9:00 move, which calibrates first

    simHook = nullptr;
    REQUIRE(t.clockModule.getHourlyExtensionPercent() == 23);
    CHECK(LinearActuator::recalibrations == 1);
    checkParkedAt(t, 23);
  }

  TEST_CASE("saved during a move, the move goes to the new position instead") {
    SolarTracker t(simMotor);
    rig(t, SolarTracker::ACTIVE, 8, 10, 0);
    simHook = [] {
      if (simPass == 50) {
        schedule(*simTracker, 6, 20, 12, 66, 18, 95, 80);
        simTracker->applySchedule();
      }
    };

    t.actuator.extendToPercent(23);

    simHook = nullptr;
    checkParkedAt(t, t.clockModule.getHourlyExtensionPercent());
  }
}
