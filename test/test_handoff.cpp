// Tests for PositionHandoff: the actuator's position carried across an OTA restart on
// the SD card, so the new firmware starts where the old one stopped instead of sweeping.
#include "../src/PositionHandoff/PositionHandoff.h"
#include <SD.h>

#include "doctest.h"

namespace mock { extern std::map<std::string, std::string> sd_files; }

TEST_SUITE("position handoff") {

  TEST_CASE("the version being installed gets the position back, once") {
    mock::reset();
    LinearActuator::Snapshot saved{790, 1000, 3600000UL, 79, true};
    REQUIRE(PositionHandoff::save("v12.6", saved, 1000000));

    LinearActuator::Snapshot got{};
    REQUIRE(PositionHandoff::take("v12.6", 1000090, got));
    CHECK(got.pos == 790);
    CHECK(got.maxPos == 1000);
    CHECK(got.sinceCalibrationMs == 3600000UL);
    CHECK(got.target == 79);
    CHECK(got.owed);

    LinearActuator::Snapshot again{};
    CHECK_FALSE(PositionHandoff::take("v12.6", 1000095, again));   // taken: a later reset recalibrates
  }

  TEST_CASE("any other version is not given the position, and the file is gone") {
    // An install that failed leaves the old firmware running, and its next reset must
    // not trust a position written for a restart that never happened.
    mock::reset();
    REQUIRE(PositionHandoff::save("v12.6", LinearActuator::Snapshot{790, 1000, 0, 79, false}, 1000000));
    LinearActuator::Snapshot got{};
    CHECK_FALSE(PositionHandoff::take("v12.3", 1000060, got));
    CHECK_FALSE(PositionHandoff::take("v12.6", 1000060, got));
  }

  TEST_CASE("no file, or a damaged one, gives nothing") {
    mock::reset();
    LinearActuator::Snapshot got{};
    CHECK_FALSE(PositionHandoff::take("v12.6", 1000000, got));
    mock::sd_files["POS.TXT"] = "v12.6 1000000 790 garbage";
    CHECK_FALSE(PositionHandoff::take("v12.6", 1000000, got));
    CHECK(mock::sd_files.count("POS.TXT") == 0);
  }

  TEST_CASE("a file older than ten minutes is not believed") {
    // A boot that could not read the card leaves the file behind; a reset days later must
    // not take a position the panel has long since left.
    mock::reset();
    REQUIRE(PositionHandoff::save("v12.6", LinearActuator::Snapshot{790, 1000, 0, 79, false}, 1000000));
    LinearActuator::Snapshot got{};
    CHECK_FALSE(PositionHandoff::take("v12.6", 1000000 + 86400, got));
    CHECK(mock::sd_files.count("POS.TXT") == 0);
  }

  TEST_CASE("an uncalibrated position is not written at all") {
    mock::reset();
    CHECK_FALSE(PositionHandoff::save("v12.6", LinearActuator::Snapshot{0, 0, 0, -1, false}, 1000000));
    CHECK(mock::sd_files.count("POS.TXT") == 0);
  }
}
