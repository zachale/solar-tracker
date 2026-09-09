// Tests for WindSpeedSensor: the analogue conversion and the hysteresis that
// decides when the array parks for high wind and when it is allowed back out.
//
// Both dwell timers are wall-clock driven, so these tests move the mock clock
// rather than waiting.
#define private public
#define protected public
#include "../src/WindSpeedSensor/WindSpeedSensor.h"
#undef private
#undef protected

#include "doctest.h"

namespace {
constexpr int WIND_PIN = A3;
constexpr int WIND_ENABLE_PIN = 4;

// The sensor reports 0.4V at rest and 2.0V at its 32 m/s ceiling.
int adcFor(float volts) { return (int)(volts / 0.004882814f + 0.5f); }
int adcForSpeed(float mps) { return adcFor(0.4f + (mps / 32.0f) * (2.0f - 0.4f)); }

WindSpeedSensor enabledSensor() {
  mock::reset();
  // The dwell timers use 0 as "not started", so millis() must not be 0 when a
  // timer is armed. See the sentinel test below.
  mock::advance(10000);
  mock::digital[WIND_ENABLE_PIN] = HIGH;
  WindSpeedSensor w;
  w.status = WindSpeedSensor::ACTIVE;
  return w;
}
} // namespace

TEST_SUITE("wind: voltage to speed") {

  TEST_CASE("at or below the floor voltage the sensor reads zero") {
    WindSpeedSensor w = enabledSensor();
    mock::analog[WIND_PIN] = adcFor(0.4f);
    CHECK(w.getSpeed() == 0);
    mock::analog[WIND_PIN] = adcFor(0.2f);
    CHECK(w.getSpeed() == 0);
    mock::analog[WIND_PIN] = 0;
    CHECK(w.getSpeed() == 0);
  }

  TEST_CASE("the full scale voltage reads the maximum speed") {
    WindSpeedSensor w = enabledSensor();
    mock::analog[WIND_PIN] = adcFor(2.0f);
    CHECK(w.getSpeed() == doctest::Approx(32).epsilon(0.05));
  }

  TEST_CASE("the conversion is linear between the endpoints") {
    WindSpeedSensor w = enabledSensor();
    for (float mps : {8.0f, 16.0f, 24.0f}) {
      CAPTURE(mps);
      mock::analog[WIND_PIN] = adcForSpeed(mps);
      CHECK(w.getSpeed() == doctest::Approx(mps).epsilon(0.08));
    }
  }

  TEST_CASE("a disabled sensor reports zero and cannot trigger safe mode") {
    mock::reset();
    mock::digital[WIND_ENABLE_PIN] = LOW;          // switch off
    WindSpeedSensor w;
    mock::analog[WIND_PIN] = adcForSpeed(30);      // a gale
    CHECK(w.getSpeed() == 0);
    CHECK(w.status == WindSpeedSensor::DISABLED);
  }
}

TEST_SUITE("wind: hysteresis") {

  TEST_CASE("a gust shorter than the dwell time does not park the array") {
    WindSpeedSensor w = enabledSensor();
    w.setUpperSpeedMax(24);
    w.setUpperSpeedDelay(60000);                   // one minute

    mock::analog[WIND_PIN] = adcForSpeed(30);      // over the threshold
    CHECK_FALSE(w.enteringHighWind());             // starts the timer
    CHECK(w.status == WindSpeedSensor::GUST_DETECTED);

    mock::advance(30000);                          // half the dwell
    CHECK_FALSE(w.enteringHighWind());

    mock::analog[WIND_PIN] = adcForSpeed(10);      // gust passes
    mock::advance(60000);
    CHECK_FALSE(w.enteringHighWind());
    CHECK(w.status != WindSpeedSensor::HIGH_WIND);
  }

  TEST_CASE("sustained wind past the dwell time parks the array") {
    WindSpeedSensor w = enabledSensor();
    w.setUpperSpeedMax(24);
    w.setUpperSpeedDelay(60000);

    mock::analog[WIND_PIN] = adcForSpeed(30);
    CHECK_FALSE(w.enteringHighWind());             // timer starts
    mock::advance(60001);
    CHECK(w.enteringHighWind());
    CHECK(w.status == WindSpeedSensor::HIGH_WIND);
  }

  TEST_CASE("release requires the lower threshold held for the lower dwell") {
    WindSpeedSensor w = enabledSensor();
    w.setLowerSpeedMax(16);
    w.setLowerSpeedDelay(300000);                  // five minutes
    w.status = WindSpeedSensor::HIGH_WIND;

    mock::analog[WIND_PIN] = adcForSpeed(10);      // calm
    CHECK_FALSE(w.exitingHighWind());              // timer starts
    mock::advance(299000);
    CHECK_FALSE(w.exitingHighWind());
    mock::advance(2000);
    CHECK(w.exitingHighWind());
    CHECK(w.status == WindSpeedSensor::ACTIVE);
  }

  TEST_CASE("the park threshold sits above the release threshold") {
    // Without a gap between them the tracker would oscillate in and out of safe
    // mode on a single sustained wind speed.
    WindSpeedSensor w = enabledSensor();
    CHECK(w.getUpperSpeedMax() > w.getLowerSpeedMax());
  }

  TEST_CASE("a dwell timer armed at millis()==0 never expires") {
    // upperSpeedTimer doubles as both the start time and the "is it running" flag,
    // so the one millisecond where millis() returns 0 arms a timer that reads as
    // unarmed and restarts on every poll. The window is tiny in the field -- the
    // first millisecond after power-on, and again on the ~49.7 day millis()
    // rollover -- but the sentinel is the reason it exists at all.
    mock::reset();                                  // millis() == 0
    mock::digital[WIND_ENABLE_PIN] = HIGH;
    WindSpeedSensor w;
    w.status = WindSpeedSensor::ACTIVE;
    w.setUpperSpeedMax(24);
    w.setUpperSpeedDelay(60000);

    mock::analog[WIND_PIN] = adcForSpeed(30);
    CHECK_FALSE(w.enteringHighWind());              // arms the timer as 0
    CHECK(w.upperSpeedTimer == 0);                  // ...which reads as unarmed

    mock::advance(120000);                          // twice the dwell
    CHECK_FALSE(w.enteringHighWind());              // still no trip
    CHECK(w.status != WindSpeedSensor::HIGH_WIND);
  }

  TEST_CASE("dwell setters and getters round-trip in milliseconds") {
    WindSpeedSensor w = enabledSensor();
    w.setUpperSpeedDelay(60000);
    w.setLowerSpeedDelay(300000);
    CHECK(w.getUpperSpeedDelay() == 60000);
    CHECK(w.getLowerSpeedDelay() == 300000);
  }
}
