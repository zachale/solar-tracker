// Tests for ClockModule's schedule interpolation.
//
// This is the maths that decides where the panel points at every hour of the day.
// It carried a variable-shadowing defect for nine days in December 2024 (fixed in
// dcde77c) which drove every morning target below zero; these tests pin the
// behaviour so that class of regression cannot return unnoticed.
#define private public
#define protected public
#include "../src/Clock/Clock.h"
#undef private
#undef protected

#include "doctest.h"

namespace {
// Builds a clock with an explicit schedule and a fixed wall time.
ClockModule at(int hour, float hStart = 8, float hMid = 12, float hFin = 20,
               float pStart = 20, float pMid = 66, float pFin = 80) {
  ClockModule c;
  c.hourStart = hStart;   c.hourMiddle = hMid;   c.hourFinish = hFin;
  c.percentStart = pStart; c.percentMiddle = pMid; c.percentFinish = pFin;
  c.rtc.mockNow = DateTime(2026, 9, 9, (uint8_t)hour, 0, 0);
  return c;
}
} // namespace

TEST_SUITE("clock: schedule interpolation") {

  TEST_CASE("the three schedule points are reproduced exactly") {
    CHECK(at(8).getHourlyExtensionPercent()  == 20);
    CHECK(at(12).getHourlyExtensionPercent() == 66);
    CHECK(at(20).getHourlyExtensionPercent() == 80);
  }

  TEST_CASE("every hour of the working day lands inside the schedule range") {
    // Interpolating between two schedule points can never leave the interval they
    // span. A result outside it means the position-along-the-window factor was not
    // between 0 and 1 — the signature of the shadowing defect.
    for (int h = 8; h <= 20; h++) {
      CAPTURE(h);
      int p = at(h).getHourlyExtensionPercent();
      CHECK(p >= 20);
      CHECK(p <= 80);
    }
  }

  TEST_CASE("the morning climbs from the start point toward the middle") {
    CHECK(at(8).getHourlyExtensionPercent()  == 20);
    CHECK(at(9).getHourlyExtensionPercent()  == 31);
    CHECK(at(10).getHourlyExtensionPercent() == 42);
    CHECK(at(11).getHourlyExtensionPercent() == 54);
  }

  TEST_CASE("the extension never decreases as the day progresses") {
    int previous = -1;
    for (int h = 8; h <= 20; h++) {
      CAPTURE(h);
      int p = at(h).getHourlyExtensionPercent();
      CHECK(p >= previous);
      previous = p;
    }
  }

  TEST_CASE("a target is never negative, whatever the schedule") {
    // A negative percentage asks the actuator for a position behind its home stop.
    // It is not rejected downstream, so it must not be produced here.
    struct Schedule { float hS, hM, hF, pS, pM, pF; };
    const Schedule schedules[] = {
      {8, 12, 20, 20, 66, 80},   // firmware defaults
      {6, 12, 16, 10, 50, 95},   // the schedule on the deployed unit's SD card
      {7, 13, 19,  5, 40, 90},
      {9, 12, 18, 30, 55, 70},
    };
    for (const auto &s : schedules) {
      for (int h = (int)s.hS; h <= (int)s.hF; h++) {
        CAPTURE(s.hS); CAPTURE(h);
        CHECK(at(h, s.hS, s.hM, s.hF, s.pS, s.pM, s.pF).getHourlyExtensionPercent() >= 0);
      }
    }
  }

  TEST_CASE("percent of day is zero at the start hour and 100 at the finish") {
    ClockModule c = at(8);
    CHECK(c.getPercentOfDay(8)  == 0);
    CHECK(c.getPercentOfDay(20) == 100);
    CHECK(c.getPercentOfDay(14) == 50);
  }

  TEST_CASE("the interval mapper stays within 0-100 inside its own window") {
    ClockModule c = at(8);
    CHECK(c.mapPercentToInterval(8, 12, c.getPercentOfDay(8))  == doctest::Approx(0.0));
    CHECK(c.mapPercentToInterval(8, 12, c.getPercentOfDay(12)) == doctest::Approx(100.0));
    float mid = c.mapPercentToInterval(8, 12, c.getPercentOfDay(10));
    CHECK(mid > 0.0f);
    CHECK(mid < 100.0f);
  }
}

TEST_SUITE("clock: safe position") {

  TEST_CASE("the stow position is a fixed setting, not a point on the curve") {
    // Safe mode must not depend on the tracking schedule: editing the schedule from
    // the dashboard should never move where the array parks in high wind.
    ClockModule a = at(10, 8, 12, 20, 20, 66, 80);
    ClockModule b = at(10, 6, 12, 16, 10, 50, 95);
    a.safeExtensionPercent = 50;
    b.safeExtensionPercent = 50;
    CHECK(a.getSafeExtensionPercent() == 50);
    CHECK(b.getSafeExtensionPercent() == 50);
    CHECK(a.getSafeExtensionPercent() == b.getSafeExtensionPercent());
  }
}

TEST_SUITE("clock: active hours") {

  TEST_CASE("active hours are inclusive of both schedule ends") {
    CHECK_FALSE(at(7).isActiveHours());
    CHECK(at(8).isActiveHours());
    CHECK(at(14).isActiveHours());
    CHECK(at(20).isActiveHours());
    CHECK_FALSE(at(21).isActiveHours());
  }
}

TEST_SUITE("clock: alarm flag") {

  TEST_CASE("the alarm flag reads true once, then clears") {
    // pollSensorData() gates the hourly move on this. It has to report the alarm
    // exactly once per firing, and it must not report one that never happened.
    ClockModule c = at(8);
    CHECK_FALSE(c.isAlarmTriggered());
    ClockModule::setAlarmTriggered();
    CHECK(c.isAlarmTriggered());
    CHECK_FALSE(c.isAlarmTriggered());
  }
}
