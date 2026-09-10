#include "./src/SettingsServer/SettingsServer.h"
#include "./src/ButtonPanel/ButtonPanel.h"
#include "./src/SolarTracker/SolarTracker.h"
#include "./src/SettingsManager/SettingsManager.h"

SolarTracker tracker(loop);
ButtonPanel buttonPanel;
SettingsServer server(&tracker);

// Throttle for the access point restore in loop(), so a radio that keeps refusing
// costs one attempt every few seconds rather than one per loop iteration.
unsigned long lastAccessPointAttempt = 0;
const unsigned long ACCESS_POINT_RETRY_INTERVAL = 10000;

void setup()
{

  Serial.begin(9600); // begining serial here because this is the first class initialized

#ifndef ESP8266
  while (!Serial)
    ; // wait for serial port to connect. Needed for native USB
#endif

  SettingsManager::begin(10);
  SettingsManager::load();

  Serial.println("Config doc:");
  String configDoc = SettingsManager::getConfigDoc();
  if (configDoc.length() > 0)
  {
    Serial.println(configDoc);
  }
  else
  {
    Serial.println("No config doc found");
  }
  buttonPanel.setup();
  tracker.setup();
  if (ButtonPanel::settingsServerEnabled())
  {
    server.start();
  }
  Serial.println("Setup complete.");
}

// The radio cannot be an access point and a station at the same time: WiFiS3
// writes +WIFIMODE=2 for beginAP() and +WIFIMODE=1 for begin(), and never the
// combined mode. So a sync means taking the access point down, talking to the
// server, and putting the access point back. That sequencing lives here because
// this is the only place that can see both the tracker and the settings server.
void loop()
{
  tracker.pollSensorData();

  if (tracker.isSyncDue())
  {
    syncWithServer();
  }

  if (ButtonPanel::settingsServerEnabled())
  {
    // Self heal. The radio sometimes needs longer than expected to accept a
    // listening socket after a mode switch, and if that happens the access point
    // has to come back on its own rather than waiting for the next sync. Without
    // this the dashboard could stay missing until somebody power cycles the unit,
    // which is the one repair that cannot be done remotely.
    if (!server.isRunning())
    {
      restoreAccessPoint();
    }
    server.checkForClient();
  }
}

void restoreAccessPoint()
{
  if (millis() - lastAccessPointAttempt < ACCESS_POINT_RETRY_INTERVAL)
  {
    return;
  }
  lastAccessPointAttempt = millis();
  Serial.println("Access point is not listening, bringing it back");
  if (!server.start())
  {
    Serial.println("Access point restore failed, retrying shortly");
  }
}

void syncWithServer()
{
  // Do not pull the access point out from under somebody who is using the
  // dashboard. The sync waits for the next opportunity instead.
  if (server.isClientConnected())
  {
    Serial.println("Skipping sync: a device is connected to the access point");
    return;
  }

  if (server.isRunning())
  {
    server.stop();
  }

  tracker.dailySync();

  // Whether the access point should exist is the switch's business, not a
  // reflection of whether the listening socket happens to be open right now. A
  // previous failed restore leaves the socket closed, and reading intent from
  // that state would mean never trying again.
  if (ButtonPanel::settingsServerEnabled())
  {
    lastAccessPointAttempt = millis();
    if (!server.start())
    {
      // Logged and left alone. loop() retries on its own, so the access point
      // comes back without waiting for the next sync.
      Serial.println("Failed to restore the access point, loop will retry");
    }
  }
}