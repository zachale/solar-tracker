#ifndef SETTINGSSERVER_H
#define SETTINGSSERVER_H

#include "WiFiS3.h"
#include <ArduinoJson.h>
#include "../SolarTracker/SolarTracker.h"
#include "../Http/UrlDecode.h"
#include "../SettingsManager/SettingsManager.h"

class SettingsServer
{
public:
  SettingsServer(SolarTracker *tracker);
  void setup();
  void checkForClient();
  void printWiFiStatus();

private:
  const char *ssid = "Solar-Tracker";
  int keyIndex = 0;
  int status = WL_IDLE_STATUS;
  SolarTracker *tracker;
  WiFiServer server;
  static const String trackerStatusStrings[];
  void endConnection(WiFiClient client);
  void getParams(String params);
  void attemptCreation();
  void sendDashboardTo(WiFiClient client);
  void actOnParameter(JsonDocument params);
};

#endif