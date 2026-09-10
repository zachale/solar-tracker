#include "./src/SettingsServer/SettingsServer.h"
#include "./src/ButtonPanel/ButtonPanel.h"
#include "./src/SolarTracker/SolarTracker.h"
#include "./src/SettingsManager/SettingsManager.h"

SolarTracker tracker(loop);
ButtonPanel buttonPanel;
SettingsServer server(&tracker);

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
    server.checkForClient();
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

  bool restoreAccessPoint = server.isRunning();
  if (restoreAccessPoint)
  {
    server.stop();
  }

  tracker.dailySync();

  // Only restore it if the switch is still on; it may have been flipped off
  // during the sync, in which case the tracker should stay a station.
  if (restoreAccessPoint && ButtonPanel::settingsServerEnabled())
  {
    if (!server.start())
    {
      // A failed restart is logged and left alone. The next sync tries again, and
      // the tracker keeps doing its real job in the meantime.
      Serial.println("Failed to restore the access point, will retry next sync");
    }
  }
}