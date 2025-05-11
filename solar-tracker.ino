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
    server.setup();
  }
  Serial.println("Setup complete.");
}

void loop()
{
  tracker.pollSensorData();
  if (ButtonPanel::settingsServerEnabled())
  {
    server.checkForClient();
  }
}