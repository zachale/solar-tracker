#include "./src/SettingsServer/SettingsServer.h"
#include "./src/ButtonPanel/ButtonPanel.h"
#include "./src/SolarTracker/SolarTracker.h"
#include "./src/SettingsManager/SettingsManager.h"
#include "./src/Watchdog/Watchdog.h"

SolarTracker tracker(loop);
ButtonPanel buttonPanel;
SettingsServer server(&tracker);

// Last acted-on position of the settings switch, so the two transitions can be
// handled rather than the position being re-read as if nothing had changed.
bool settingsSwitchWasOn = false;

// A mechanical switch bounces, and a bounce acted on bare would tear the radio
// down and build it back up several times in one second. Observed on hardware:
// three transitions inside one second. A new position has to hold this long
// before it counts.
bool settingsSwitchCandidate = false;
unsigned long settingsSwitchSince = 0;
const unsigned long SETTINGS_SWITCH_DEBOUNCE = 500;

// The card is the only place telemetry and WiFi credentials live, so losing it is
// worth shouting about and worth retrying. Without the retry a card that comes
// back needs a power cycle, which is the one repair nobody can do remotely.
bool sdReady = false;
unsigned long lastSdRetry = 0;
const unsigned long SD_RETRY_INTERVAL = 30000;
const uint8_t SD_CHIP_SELECT = 10;

bool beginStorage()
{
  if (!SettingsManager::begin(SD_CHIP_SELECT))
  {
    Serial.println("SD CARD UNAVAILABLE: telemetry cannot be written and WiFi credentials cannot be read");
    return false;
  }
  SettingsManager::load();
  Serial.println("Config doc:");
  String configDoc = SettingsManager::getConfigDoc();
  if (configDoc.length() > 0 && configDoc != "null")
  {
    Serial.println(configDoc);
  }
  else
  {
    Serial.println("SD CARD READABLE BUT CONFIG IS EMPTY: no saved settings or WiFi credentials");
  }
  return true;
}

void setup()
{

  Serial.begin(9600); // begining serial here because this is the first class initialized

  // Before anything that can hang. Everything after this line is covered: a stall longer
  // than the loop deadline, or a hard fault, resets the board, and a new image that keeps
  // doing that is rolled back by tracker.otaBoot() below.
  Watchdog::begin();

  // A fixed pause, not `while (!Serial)`. On this board Serial reads true without a
  // host attached, so the old wait fell straight through and everything printed here
  // was gone before a capture could open the port. Five seconds is enough to start one.
  delay(5000);

  // Which bridge firmware the ESP32 is running. Read-only: +FWVERSION and
  // +FWVERSION_U32 are queries, so this cannot change anything on either chip.
  // The U32 form is the one to compare against; the string is for the log.
  Serial.println("Firmware version: " FIRMWARE_VERSION);
  // Printed again here: begin() ran before the pause, before any capture could hear it.
  Serial.println(Watchdog::describe());
  Serial.print("ESP32 bridge firmware: ");
  Serial.println(WiFi.firmwareVersion());
  Serial.print("ESP32 bridge firmware U32: 0x");
  Serial.println(WiFi.firmwareVersionU32(), HEX);

  // Trial bookkeeping for over-the-air updates, as early as possible: a new image that
  // hangs or crashes further down still gets this far, so a crash loop is counted and
  // rolled back before it can repeat for ever. Needs the modem, which the version query
  // above has just started.
  tracker.otaBoot(SD_CHIP_SELECT);

#if defined(OTA_FAULT_HANG_SETUP)
  Serial.println("FAULT INJECTION: hanging in setup()");
  while (true)
  {
  }
#elif defined(OTA_FAULT_CRASH_SETUP)
  Serial.println("FAULT INJECTION: hard fault in setup()");
  Serial.flush();
  __builtin_trap(); // an undefined instruction: a fault on every Cortex-M, guaranteed
#endif

  sdReady = beginStorage();

  buttonPanel.setup();

  // Settle the switch before tracker.setup(), because that runs the first sync and
  // the sync has to already know whether it is allowed to use the radio.
  settingsSwitchWasOn = ButtonPanel::settingsServerEnabled();
  settingsSwitchCandidate = settingsSwitchWasOn;
  tracker.setAppMode(settingsSwitchWasOn);

  tracker.setup();

  if (settingsSwitchWasOn)
  {
    Serial.println("App mode: serving the dashboard, telemetry is spooled but not shipped");
    server.start();
  }
  else
  {
    Serial.println("Field mode: no access point, telemetry will be shipped on schedule");
  }
  Serial.println("Setup complete.");
}

// The radio can only do one job at a time: WiFiS3 writes +WIFIMODE=2 for
// beginAP() and +WIFIMODE=1 for begin(), never the combined mode. So while the
// settings switch is on the tracker serves the dashboard and ships nothing, and
// when it is off there is no access point and telemetry goes to the server.
//
// The switch is honoured in both directions, because the normal way to get a
// dashboard is to walk up to a running tracker and flip it on.
void loop()
{
  Watchdog::progress();
  tracker.pollSensorData();

  if (!sdReady && millis() - lastSdRetry > SD_RETRY_INTERVAL)
  {
    lastSdRetry = millis();
    sdReady = beginStorage();
  }

  // A download in flight is let finish before the switch is acted on. Bringing the access
  // point up starts with WiFi.end(), which would kill the transfer and leave the updater
  // waiting on a download that had already died. The wait is seconds: a whole image comes
  // down in a few, and a stalled one gives up after a minute.
  if (readSettingsSwitch() != settingsSwitchWasOn && !tracker.otaBusy())
  {
    settingsSwitchWasOn = !settingsSwitchWasOn;
    onSettingsSwitchChanged(settingsSwitchWasOn);
  }

  if (settingsSwitchWasOn)
  {
    server.checkForClient();
  }
}

// Debounced read. The raw pin has to agree with itself for SETTINGS_SWITCH_DEBOUNCE
// before a new position is reported.
bool readSettingsSwitch()
{
  bool raw = ButtonPanel::settingsServerEnabled();
  if (raw != settingsSwitchCandidate)
  {
    settingsSwitchCandidate = raw;
    settingsSwitchSince = millis();
    return settingsSwitchWasOn;
  }
  if (raw != settingsSwitchWasOn && millis() - settingsSwitchSince > SETTINGS_SWITCH_DEBOUNCE)
  {
    return raw;
  }
  return settingsSwitchWasOn;
}

void onSettingsSwitchChanged(bool isOn)
{
  // The tracker has to know before anything else, so a sync already queued cannot
  // reach for a radio that is about to become an access point.
  tracker.setAppMode(isOn);

  if (isOn)
  {
    Serial.println("Settings switch turned on, starting the access point");
    // Release the radio first. A station association holds a socket, and beginAP()
    // without this is the case that stops working after four tries; with it the
    // same test ran twelve for twelve reusing one socket.
    server.stop();
    WiFi.end();
    if (!server.start())
    {
      Serial.println("Access point failed to start, will retry on the next switch read");
      settingsSwitchWasOn = false;
    }
    return;
  }

  Serial.println("Settings switch turned off, releasing the access point");
  // Closing the listener keeps this object honest about not running. The socket
  // itself comes back on the WiFi.end() inside the next station association.
  server.stop();
}
