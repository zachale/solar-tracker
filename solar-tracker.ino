#include "./src/SettingsServer/SettingsServer.h"
#include "./src/ButtonPanel/ButtonPanel.h"
#include "./src/SolarTracker/SolarTracker.h"

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