#ifndef W_H
#define W_H

#include "WiFiS3.h"
#include <ArduinoJson.h>

#include "../SolarTracker/SolarTracker.h"

class WifiModule
{
public:
  WifiModule(SolarTracker *tracker);
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
  void attemptConnection(char *, char *);
  void attemptCreation();
  void sendDashboardTo(WiFiClient client);
  void actOnParameter(JsonDocument params);
};

#endif