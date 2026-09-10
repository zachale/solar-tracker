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

// The access point is brought up once, at boot, and never taken down. The radio
// cannot be an access point and a station at the same time, and the ESP32 will
// only accept four access point bring-ups per power session before it stops
// handing out listening sockets altogether, measured on this hardware. So the
// tracker does not cycle it: while the settings switch is on it spools telemetry
// and talks to nobody, and all server traffic waits until the switch is off.
void loop()
{
  tracker.pollSensorData();

  if (ButtonPanel::settingsServerEnabled())
  {
    server.checkForClient();
  }
}
