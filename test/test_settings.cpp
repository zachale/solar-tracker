// Tests for SettingsManager: the SD-backed config that holds the schedule, the
// wind thresholds and the WiFi credentials. Backed here by an in-memory card.
#define private public
#define protected public
#include "../src/SettingsManager/SettingsManager.h"
#undef private
#undef protected

#include "doctest.h"
#include <SD.h>

TEST_SUITE("settings: persistence") {

  TEST_CASE("a saved setting survives a reload") {
    mock::reset();
    REQUIRE(SettingsManager::begin(4));
    SettingsManager::upsertSetting((char *)"windUpperMaxSpeed", 5.5f);
    SettingsManager::upsertSetting((char *)"windUpperWait", 60000);
    REQUIRE(SettingsManager::save());

    SettingsManager::configDocument.clear();
    REQUIRE(SettingsManager::load());
    CHECK(SettingsManager::getSetting<float>((char *)"windUpperMaxSpeed") == doctest::Approx(5.5));
    CHECK(SettingsManager::getSetting<int>((char *)"windUpperWait") == 60000);
  }

  TEST_CASE("upsert overwrites rather than duplicating") {
    mock::reset();
    REQUIRE(SettingsManager::begin(4));
    SettingsManager::upsertSetting((char *)"safePercent", 50);
    SettingsManager::upsertSetting((char *)"safePercent", 80);
    CHECK(SettingsManager::getSetting<int>((char *)"safePercent") == 80);
  }

  TEST_CASE("a missing card is reported rather than silently succeeding") {
    mock::reset();
    mock::sd_ok = false;
    CHECK_FALSE(SettingsManager::begin(4));
  }
}

TEST_SUITE("settings: wifi credentials") {

  TEST_CASE("credentials round-trip through the card") {
    mock::reset();
    REQUIRE(SettingsManager::begin(4));
    SettingsManager::wifiCredentials.clear();
    SettingsManager::addWifiCredential("Rock Lodge", "hunter2");
    SettingsManager::addWifiCredential("uppergrand-5", "hunter3");
    REQUIRE(SettingsManager::saveWifiCredentials());

    SettingsManager::wifiCredentials.clear();
    REQUIRE(SettingsManager::loadWifiCredentials());
    auto creds = SettingsManager::getWifiCredentials();
    REQUIRE(creds.size() == 2);
    CHECK(creds[0].first == String("Rock Lodge"));
    CHECK(creds[1].first == String("uppergrand-5"));
  }

  TEST_CASE("removing a credential leaves the others intact") {
    mock::reset();
    REQUIRE(SettingsManager::begin(4));
    SettingsManager::wifiCredentials.clear();
    SettingsManager::addWifiCredential("a", "1");
    SettingsManager::addWifiCredential("b", "2");
    SettingsManager::addWifiCredential("c", "3");
    SettingsManager::removeWifiCredential(1);
    auto creds = SettingsManager::getWifiCredentials();
    REQUIRE(creds.size() == 2);
    CHECK(creds[0].first == String("a"));
    CHECK(creds[1].first == String("c"));
  }

  TEST_CASE("removing an out of range index does not corrupt the list") {
    mock::reset();
    REQUIRE(SettingsManager::begin(4));
    SettingsManager::wifiCredentials.clear();
    SettingsManager::addWifiCredential("only", "1");
    SettingsManager::removeWifiCredential(7);
    CHECK(SettingsManager::getWifiCredentials().size() == 1);
  }
}

TEST_SUITE("settings: filenames") {

  TEST_CASE("every storage filename fits the SD library's 8.3 limit") {
    // The Arduino SD library uses the legacy short-name FAT API. A longer name
    // fails at SD.open() in both directions, and the firmware reports it as
    // "error opening ... file" — which reads like a card fault rather than a
    // name that was never openable.
    CHECK(isValid83(SettingsManager::storageFile));
    CHECK(isValid83(SettingsManager::wifiStorageFile));
  }

  TEST_CASE("the 8.3 check itself is right") {
    CHECK(isValid83("config"));
    CHECK(isValid83("wifi.jsn"));
    CHECK(isValid83("12345678.abc"));
    CHECK_FALSE(isValid83("wifi_credentials.json"));
    CHECK_FALSE(isValid83("123456789"));          // nine characters
    CHECK_FALSE(isValid83("name.abcd"));          // four character extension
    CHECK_FALSE(isValid83("two.dots.x"));
    CHECK_FALSE(isValid83(""));
  }
}
