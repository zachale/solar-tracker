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
  // Brings the access point up and starts listening. Returns false rather than
  // hanging if the radio refuses, so a failed restart costs one cycle instead of
  // wedging a device that cannot be serviced remotely.
  bool start();
  // Closes the listening socket. The radio is left alone; whoever calls this is
  // about to hand it to the station path.
  void stop();
  bool isRunning();
  void checkForClient();
  void printWiFiStatus();

private:
  const char *ssid = "Solar-Tracker";
  int keyIndex = 0;
  int status = WL_IDLE_STATUS;
  // The radio can take over ten seconds to accept a listening socket after a
  // mode switch. Observed failing at five seconds on hardware.
  static const unsigned long AP_START_TIMEOUT = 20000;
  SolarTracker *tracker;
  WiFiServer server;
  static const String trackerStatusStrings[];
  void endConnection(WiFiClient client);
  void getParams(String params);
  bool attemptCreation();
  void sendDashboardTo(WiFiClient client);
  void actOnParameter(JsonDocument params);
};

#endif