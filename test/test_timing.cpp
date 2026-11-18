// Tests for the sweep timing that rides along with every sync (LinearActuator::legsQuery),
// and for the time limit that keeps a sweep from running for ever.
#define private public
#define protected public
#include "../src/LinearActuator/LinearActuator.h"
#undef private
#undef protected

#include "doctest.h"
#include <string>

namespace {
// A motor on the bench, as in test_actuator.cpp: 20 ms a pass, one or two hall pulses a pass
// while driven and short of an end stop. `noisy` keeps the pulses coming at the end stops too,
// like a hall input left floating.
LinearActuator *timedActuator = nullptr;
long timedPos = 0;
const long TIMED_LIMIT = 1000;
bool noisy = false;
long timedPass = 0;

void timedMotor() {
  mock::advance(20);
  int pulses = (++timedPass % 3 == 0) ? 2 : 1;
  LinearActuator &a = *timedActuator;
  bool driven = a.status != 0;
  bool forward = driven && a.dir > 0;
  bool backward = driven && a.dir < 0;
  for (int i = 0; i < pulses; i++) {
    bool moves = (forward && timedPos < TIMED_LIMIT) || (backward && timedPos > 0);
    if (moves) {
      timedPos += forward ? 1 : -1;
    }
    if (moves || (driven && noisy)) {
      mock::now_us += 600;
      LinearActuator::countSteps();
    }
  }
}

LinearActuator &timedBench(LinearActuator &a, long at) {
  mock::reset();
  mock::advance(10000);
  timedActuator = &a;
  timedPos = at;
  noisy = false;
  LinearActuator::steps = 0;
  LinearActuator::lastStepTime = 0;
  LinearActuator::recalibrations = 0;
  LinearActuator::moveTimeouts = 0;
  a.maxPos = 1000;
  a.pos = at;
  a.setExtensionCallBack(timedMotor);
  return a;
}

int count(const std::string &s, char c) {
  int n = 0;
  for (char x : s) n += x == c;
  return n;
}
} // namespace

TEST_SUITE("actuator: sweep timing") {

  TEST_CASE("a home leg that reaches its end records its time, pulses, passes and a gap over a second") {
    LinearActuator a;
    timedBench(a, 300);
    a.home();
    REQUIRE(a.legsRecorded == 1);
    const LinearActuator::Leg &leg = a.legLog[0];
    CHECK(leg.kind == 'H');
    CHECK(leg.pulses == 300);
    CHECK(leg.passes > 0);
    CHECK(leg.ms >= 1000);
    const unsigned long limit = LinearActuator::SWEEP_TIMEOUT_MS; // a copy: no out-of-class definition
    CHECK(leg.ms < limit);
    CHECK(leg.gapMs >= 1000); // ended because the pulses had stopped for a second
    CHECK(timedPos == 0);
    CHECK(a.pos == 0);
  }

  TEST_CASE("a max leg records the stroke it measured") {
    LinearActuator a;
    timedBench(a, 0);
    a.pos = 0;
    a.max();
    REQUIRE(a.legsRecorded == 1);
    const LinearActuator::Leg &leg = a.legLog[0];
    CHECK(leg.kind == 'M');
    CHECK(leg.pulses == a.maxPos);
    CHECK(a.maxPos == doctest::Approx(1000).epsilon(0.01));
    CHECK(leg.gapMs >= 1000);
  }

  TEST_CASE("a sweep whose pulses never stop ends at the time limit, stopped, and says so") {
    LinearActuator a;
    timedBench(a, 300);
    noisy = true;
    a.home();
    REQUIRE(a.legsRecorded == 1);
    CHECK(a.legLog[0].kind == 'h');
    const unsigned long limit = LinearActuator::SWEEP_TIMEOUT_MS;
    CHECK(a.legLog[0].ms > limit);
    CHECK(a.legLog[0].gapMs < 1000); // pulses were still arriving
    const int stopped = LinearActuator::STOP;
    CHECK(a.status == stopped);
    CHECK(mock::pwm[5] == 0);
    CHECK(mock::pwm[6] == 0);

    a.max();
    CHECK(a.legLog[1].kind == 'm');
    CHECK(a.status == stopped);
  }

  TEST_CASE("the query carries the frame and the legs, oldest first") {
    LinearActuator a;
    timedBench(a, 300);
    a.recalibrate();
    std::string q = a.legsQuery().s;
    INFO(q);
    CHECK(q.rfind("&mp=" + std::to_string(a.maxPos) + "&p=" + std::to_string(a.pos) + "&mto=0&legn=2&legs=H", 0) == 0);
    CHECK(q.find("_M") != std::string::npos);
    CHECK(q.find(".300.") != std::string::npos); // the home leg's pulses
    // Only characters a URL carries as they are.
    for (char c : q) {
      bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   c == '&' || c == '=' || c == '.' || c == '_' || c == '-';
      CHECK(plain);
    }
  }

  TEST_CASE("before any sweep the query has the frame and no legs") {
    LinearActuator a;
    timedBench(a, 300);
    CHECK(a.legsQuery().s == "&mp=1000&p=300&mto=0&legn=0");
  }

  TEST_CASE("only the last eight legs are kept, and legn counts them all") {
    LinearActuator a;
    timedBench(a, 300);
    for (int i = 0; i < 5; i++) {
      a.recalibrate();
    }
    std::string q = a.legsQuery().s;
    INFO(q);
    CHECK(a.legsRecorded == 10);
    CHECK(q.find("&legn=10&legs=") != std::string::npos);
    CHECK(count(q, '_') == 7); // eight legs
  }
}
