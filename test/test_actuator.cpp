// Tests for LinearActuator's position arithmetic and stall detection.
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
    // A stalled motor that vibrates its own hall sensor therefore reads as alive,
    // which is what allows the max() loop to spin without an exit.
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
