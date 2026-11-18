// Tests for LinearActuator's position arithmetic, stall detection and move limits.
//
// Access control is relaxed for this translation unit only. The position maths and
// the boundary detector are private, and they are exactly the parts worth pinning
// down, so the alternative would be widening the firmware's public surface purely
// for the benefit of tests.
#define private public
#define protected public
#include "../src/LinearActuator/LinearActuator.h"
#undef private
#undef protected

#include "doctest.h"

namespace {
// Puts an actuator into a calibrated state without needing a motor to move.
LinearActuator calibrated(long maxPos, long pos) {
  LinearActuator a;
  a.maxPos = maxPos;
  a.pos = pos;
  return a;
}
} // namespace

TEST_SUITE("actuator: position maths") {

  TEST_CASE("percent extended is derived from pos over maxPos") {
    LinearActuator a = calibrated(20000, 5000);
    CHECK(a.getPercentExtended() == 25);

    a.pos = 0;
    CHECK(a.getPercentExtended() == 0);

    a.pos = 20000;
    CHECK(a.getPercentExtended() == 100);
  }

  TEST_CASE("percent extended reports -1 before calibration") {
    LinearActuator a;
    REQUIRE(a.maxPos == 0);
    CHECK(a.getPercentExtended() == -1);
  }

  TEST_CASE("percent to pulses round-trips") {
    LinearActuator a = calibrated(25000, 0);
    CHECK(a.getPosFromPercent(0) == 0);
    CHECK(a.getPosFromPercent(50) == 12500);
    CHECK(a.getPosFromPercent(100) == 25000);
  }

  TEST_CASE("a negative target maps to a position behind the home stop") {
    // Any schedule that yields a negative percentage asks the actuator to travel
    // past its own end stop. Nothing in extendToPercent() rejects it: the 0 and 100
    // special cases do not match, so it becomes an ordinary move toward a
    // position that cannot exist.
    LinearActuator a = calibrated(25000, 12000);
    int target = a.getPosFromPercent(-2);
    CHECK(target < 0);
    CHECK(target == -500);
  }
}

TEST_SUITE("actuator: stall detection") {

  TEST_CASE("boundary is reported after a second of no pulses") {
    mock::reset();
    LinearActuator a = calibrated(20000, 0);
    LinearActuator::steps = 100;
    a.prevSteps = 100;
    a.prevTimer = millis();

    CHECK_FALSE(a.hitBoundary());     // same instant

    mock::advance(999);
    CHECK_FALSE(a.hitBoundary());     // just short of the threshold

    mock::advance(2);
    CHECK(a.hitBoundary());           // past 1000ms with no new pulses
  }

  TEST_CASE("a single stray pulse restarts the stall timer") {
    // The detector treats any change in the step count as evidence of movement.
    // A stalled motor that vibrates its own hall sensor therefore reads as alive.
    mock::reset();
    LinearActuator a = calibrated(20000, 0);
    LinearActuator::steps = 100;
    a.prevSteps = 100;
    a.prevTimer = millis();

    mock::advance(900);
    LinearActuator::steps = 101;      // one spurious count
    CHECK_FALSE(a.hitBoundary());     // timer resets here

    mock::advance(900);               // 1800ms total, still no boundary
    CHECK_FALSE(a.hitBoundary());
  }

  TEST_CASE("a pulse stamped after the clock was read cannot end a sweep") {
    // The v12.11-v12.13 fault. Their check read micros() and then the interrupt's lastStepTime;
    // a pulse in between left a stamp newer than the clock reading, the unsigned difference
    // wrapped to about 71 minutes, and the sweep ended with the motor still running. The
    // check compares pulse counts again, so a stamp in the future means nothing to it.
    mock::reset();
    mock::advance(5000);
    LinearActuator a = calibrated(20000, 0);
    a.prevTimer = millis() - 2000;              // driving for two seconds
    LinearActuator::steps = 100;
    a.prevSteps = 99;                           // and the count moved since the last pass
    LinearActuator::lastStepTime = micros() + 5;
    CHECK_FALSE(a.hitBoundary());
  }

  TEST_CASE("step counting debounces below the trigger delay") {
    mock::reset();
    LinearActuator::steps = 0;
    LinearActuator::lastStepTime = 0;

    mock::now_us = 1000;
    LinearActuator::countSteps();
    CHECK(LinearActuator::steps == 1);

    mock::now_us += 100;              // 100us later, inside the 500us window
    LinearActuator::countSteps();
    CHECK(LinearActuator::steps == 1);

    mock::now_us += 600;              // outside the window
    LinearActuator::countSteps();
    CHECK(LinearActuator::steps == 2);
  }
}

namespace {
// A motor on the bench: one hall pulse per loop pass while it is driven and not against
// an end stop, 20 ms a pass. `blockAt` stands in for an obstruction partway along.
LinearActuator *simActuator = nullptr;
long simPos = 0;
long simLimit = 1000;
long simBlockAt = -1;

long simPass = 0;

void simMotor() {
  mock::advance(20);
  // One or two pulses a pass, each through the interrupt handler as on the board.
  int pulses = (++simPass % 3 == 0) ? 2 : 1;
  LinearActuator &a = *simActuator;
  // Driven whenever it is not stopped, in the direction it was last set to.
  bool driven = a.status != 0;
  bool forward = driven && a.dir > 0;
  bool backward = driven && a.dir < 0;
  for (int i = 0; i < pulses; i++) {
    bool moves = (forward && simPos < simLimit && simPos != simBlockAt) ||
                 (backward && simPos > 0 && simPos != simBlockAt);
    if (moves) { simPos += forward ? 1 : -1; mock::now_us += 600; LinearActuator::countSteps(); }
  }
}

// An actuator wired to the simulated motor, sitting at `at` pulses out of 1000.
LinearActuator &onBench(LinearActuator &a, long at, bool calibrated) {
  mock::reset();
  mock::advance(10000);
  simActuator = &a;
  simPos = at;
  simLimit = 1000;
  simBlockAt = -1;
  LinearActuator::steps = 0;
  LinearActuator::recalibrations = 0;
  LinearActuator::lastRecalibrationTime = millis();
  a.isHomed = 0;
  a.calibrationRequested = false;
  a.calibrationAllowed = true;
  a.currentPercentTarget = -1;
  if (calibrated) { a.maxPos = 1000; a.pos = at; }
  a.setExtensionCallBack(simMotor);
  return a;
}

constexpr unsigned long WEEK_MS = 604800000UL;
} // namespace

TEST_SUITE("actuator: when it may calibrate") {

  TEST_CASE("with the position unknown, SAFE holds still rather than sweep in the wind") {
    LinearActuator a;
    onBench(a, 800, false);
    a.calibrationAllowed = false;
    a.positionCalibrationAllowed = false;
    a.extendToPercent(80);
    CHECK(simPos == 800);
    CHECK(LinearActuator::recalibrations == 0);
    CHECK(a.calibrationDue());
    CHECK(a.currentPercentTarget == -1);   // so the same request goes through once it may
  }

  TEST_CASE("with the position unknown, away calibrates once and goes to the away position") {
    // A boot in away: without a position the panel reads -1, which nobody can make sense
    // of, so the one calibration that finds it is allowed even in away.
    LinearActuator a;
    onBench(a, 300, false);
    a.calibrationAllowed = false;            // away: no weekly calibration
    a.positionCalibrationAllowed = true;     // but finding the position is allowed
    a.extendToPercent(80);
    CHECK(LinearActuator::recalibrations == 1);
    CHECK(a.getPercentExtended() == doctest::Approx(80).epsilon(0.02));
    CHECK(simPos == doctest::Approx(800).epsilon(0.02));
  }

  TEST_CASE("in away with the position known, a due weekly calibration still waits") {
    LinearActuator a;
    onBench(a, 790, true);
    mock::advance(WEEK_MS + 1);
    a.calibrationAllowed = false;
    a.positionCalibrationAllowed = true;
    a.extendToPercent(80);
    CHECK(LinearActuator::recalibrations == 0);
    CHECK(a.calibrationDue());
  }

  TEST_CASE("with the position unknown, a move that may calibrate calibrates first") {
    LinearActuator a;
    onBench(a, 300, false);
    a.calibrationAllowed = true;
    a.extendToPercent(50);
    CHECK(LinearActuator::recalibrations == 1);
    CHECK(a.maxPos == 1000);
    CHECK(simPos == doctest::Approx(500).epsilon(0.02));
    CHECK_FALSE(a.calibrationDue());
  }

  TEST_CASE("a week after the last calibration it is due, and stays due through moves that may not") {
    LinearActuator a;
    onBench(a, 500, true);
    mock::advance(WEEK_MS + 1);
    CHECK(a.calibrationDue());

    a.calibrationAllowed = false;

    a.extendToPercent(80);           // away mode: go to the away position, no sweep
    CHECK(LinearActuator::recalibrations == 0);
    CHECK(simPos == doctest::Approx(800).epsilon(0.02));
    mock::advance(WEEK_MS);                 // a long time away
    CHECK(a.calibrationDue());              // still ringing

    a.calibrationAllowed = true;

    a.extendToPercent(40);            // out of away: calibrate first, then move
    CHECK(LinearActuator::recalibrations == 1);
    CHECK(simPos == doctest::Approx(400).epsilon(0.02));
    CHECK_FALSE(a.calibrationDue());
  }

  TEST_CASE("a move that runs into something stops at its time limit and holds there") {
    // The pulses it counted are still right, so nothing is calibrated for it.
    LinearActuator a;
    onBench(a, 200, true);
    LinearActuator::moveTimeouts = 0;
    simBlockAt = 400;
    a.extendToPercent(80);
    CHECK(LinearActuator::recalibrations == 0);
    CHECK(LinearActuator::moveTimeouts == 1);
    CHECK(simPos == 400);
    CHECK(a.pos == 400);
    CHECK_FALSE(a.calibrationDue());
    CHECK(a.currentPercentTarget == -1);   // so the same request is tried again
    const int stopped = LinearActuator::STOP;
    CHECK(a.status == stopped);
    CHECK(mock::pwm[5] == 0);
    CHECK(mock::pwm[6] == 0);
  }

  TEST_CASE("a request made during the sweep is where the panel goes, not the hourly one") {
    // The weekly sweep before an hourly move calls back into the main loop; if the wind
    // rises or the away switch goes on meanwhile, the safe position is asked for then.
    LinearActuator a;
    onBench(a, 500, true);
    mock::advance(WEEK_MS + 1);
    static LinearActuator *redirect = nullptr;
    static int passes = 0;
    redirect = &a;
    passes = 0;
    a.setExtensionCallBack([] {
      simMotor();
      if (++passes == 30) {                         // partway home: away switch on
        redirect->calibrationAllowed = false;
        redirect->extendToPercent(80);
      }
    });
    a.extendToPercent(40);
    CHECK(LinearActuator::recalibrations == 1);
    CHECK(simPos == doctest::Approx(800).epsilon(0.02));
  }

  TEST_CASE("a calibration that came due stays due past the 49.7 day millis wrap") {
    LinearActuator a;
    onBench(a, 500, true);
    mock::advance(WEEK_MS + 1);
    CHECK(a.calibrationDue());                      // latched here
    // On the board millis() is 32 bits and the elapsed time wraps to near zero after 49.7
    // days; unsigned long is 64 bits here, so the wrap is made by hand.
    LinearActuator::lastRecalibrationTime = millis();
    CHECK(a.calibrationDue());
  }

  TEST_CASE("calibrating in place goes back to the position it was asked to hold") {
    LinearActuator a;
    onBench(a, 800, true);
    a.calibrationAllowed = false;
    a.extendToPercent(80);
    a.recalibrateInPlace(95);
    CHECK(LinearActuator::recalibrations == 1);
    CHECK(simPos == doctest::Approx(800).epsilon(0.02));
  }

  TEST_CASE("calibrating in place with nothing to go back to goes to the fallback") {
    LinearActuator a;
    onBench(a, 300, false);
    a.recalibrateInPlace(95);
    CHECK(LinearActuator::recalibrations == 1);
    CHECK(simPos == doctest::Approx(950).epsilon(0.02));
  }
}

TEST_SUITE("actuator: snapshot across a restart") {

  TEST_CASE("a snapshot restores the position, the target and how long since the last calibration") {
    LinearActuator a;
    onBench(a, 790, true);
    a.calibrationAllowed = false;
    a.extendToPercent(79);
    mock::advance(6 * 24 * 3600000UL);      // six days since the last calibration
    LinearActuator::Snapshot s = a.snapshot();

    LinearActuator b;                       // the new firmware, freshly booted
    mock::reset();
    mock::advance(30000);
    LinearActuator::lastRecalibrationTime = millis();
    REQUIRE(b.restore(s));
    CHECK(b.getPercentExtended() == a.getPercentExtended());
    CHECK(b.currentPercentTarget == 79);    // the away move after boot is a no-op
    CHECK_FALSE(b.calibrationDue());
    mock::advance(24 * 3600000UL + 1000);   // the seventh day
    CHECK(b.calibrationDue());
  }

  TEST_CASE("a calibration that was owed is still owed after the restart") {
    LinearActuator a;
    onBench(a, 500, true);
    a.calibrationRequested = true;
    LinearActuator::Snapshot s = a.snapshot();
    LinearActuator b;
    REQUIRE(b.restore(s));
    CHECK(b.calibrationDue());
  }

  TEST_CASE("a snapshot without a calibrated position is not restored") {
    LinearActuator b;
    LinearActuator::Snapshot s{0, 0, 0, -1, false};
    CHECK_FALSE(b.restore(s));
    CHECK(b.getPercentExtended() == -1);
  }
}

TEST_SUITE("actuator: moves that cannot finish") {

  TEST_CASE("a target past the end stop goes to the end and stops") {
    // What the evening alarm used to ask for: a position past full extension. It is taken as
    // full extension, which max() finds by its end stop.
    LinearActuator a;
    onBench(a, 900, true);
    a.extendToPercent(103);
    const int stopped = LinearActuator::STOP;
    CHECK(a.status == stopped);
    CHECK(mock::pwm[5] == 0);
    CHECK(mock::pwm[6] == 0);
    CHECK(simPos == 1000);
  }

  TEST_CASE("a target below zero goes home") {
    LinearActuator a;
    onBench(a, 300, true);
    a.extendToPercent(-4);
    CHECK(simPos == 0);
    CHECK(a.pos == 0);
    const int stopped = LinearActuator::STOP;
    CHECK(a.status == stopped);
  }

  TEST_CASE("a move redirected past the end stops at its time limit") {
    // A redirect sets the target directly, around the clamp: the motor reaches its end stop
    // and the move can never finish.
    LinearActuator a;
    onBench(a, 500, true);
    LinearActuator::moveTimeouts = 0;
    static LinearActuator *redirect = nullptr;
    static int passes = 0;
    redirect = &a;
    passes = 0;
    a.setExtensionCallBack([] {
      simMotor();
      if (++passes == 50) redirect->currentPercentTarget = 105;   // past the end, mid-move
    });
    a.extendToPercent(90);
    const int stopped = LinearActuator::STOP;
    CHECK(a.status == stopped);
    CHECK(simPos == 1000);
    CHECK(LinearActuator::moveTimeouts == 1);
    CHECK(mock::pwm[5] == 0);
    CHECK(mock::pwm[6] == 0);
  }

  TEST_CASE("the time limit starts again when the target changes") {
    // Stuck at the end stop for almost the whole limit, then SAFE asks for the middle: the new
    // target gets a full window of its own, and the panel gets there.
    LinearActuator a;
    onBench(a, 900, true);
    LinearActuator::moveTimeouts = 0;
    static LinearActuator *redirect = nullptr;
    static unsigned long startedAt = 0;
    static bool redirected = false;
    redirect = &a;
    startedAt = millis();
    redirected = false;
    a.setExtensionCallBack([] {
      simMotor();
      const unsigned long limit = LinearActuator::MOVE_TIMEOUT_MS;
      if (!redirected && millis() - startedAt > limit - 1000) {
        redirected = true;
        redirect->currentPercentTarget = 50;
      }
    });
    a.currentPercentTarget = 105;            // what the old evening alarm asked for, unclamped
    bool reached = a.moveTo(105);
    CHECK(reached);
    CHECK(redirected);
    CHECK(simPos == doctest::Approx(500).epsilon(0.02));
    CHECK(LinearActuator::moveTimeouts == 0);
  }
}

TEST_SUITE("actuator: recalibration") {

  TEST_CASE("a recalibration asked for in the middle of a slow approach sweeps at full speed") {
    // The dashboard's button runs recalibrate() from inside a move's callback.
    LinearActuator a;
    onBench(a, 500, true);
    static int slowestHome = 255;
    slowestHome = 255;
    a.setExtensionCallBack([] {
      simMotor();
      if (simActuator->status == LinearActuator::HOMING && mock::pwm[5] < slowestHome) {
        slowestHome = mock::pwm[5];
      }
    });
    a.speed = 120;                            // the approach speed of a move near its target
    a.extend(LinearActuator::FORWARD);
    a.recalibrate();
    CHECK(slowestHome == 255);
    CHECK(simPos == 1000);
    CHECK(a.maxPos == doctest::Approx(1000).epsilon(0.01));
  }
}

TEST_SUITE("actuator: homing") {

  TEST_CASE("every home() drives back to the stop, not only the first") {
    // isHomed stays 1 after the first homing. home() has to clear it, or every later
    // recalibration skips the drive home and keeps a position that has drifted.
    LinearActuator a;
    onBench(a, 500, true);
    a.isHomed = 1;                                   // left over from an earlier homing

    a.home();

    CHECK(simPos == 0);
    CHECK(a.pos == 0);
    CHECK(a.isHomed);
    const int stopped = LinearActuator::STOP;  // a copy: the constant has no out-of-class definition
    CHECK(a.status == stopped);
  }
}
