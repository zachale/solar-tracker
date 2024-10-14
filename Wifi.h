#ifndef W_H
#define W_H

#include "WiFiS3.h"
#include <ArduinoJson.h>

#include "./LinearActuator.h" 
#include "./WindSpeedSensor.h" 
#include "./Clock.h"
#include "./secrets.h"


class WifiModule {
  public:
    WifiModule(int* trackerStatus, LinearActuator*, WindSpeedSensor*, ClockModule*);
    void setup();
    void checkForClient();
    void printWiFiStatus();
  private:
    const char *ssid = "Solar-Tracker";
    int keyIndex = 0;                 
    int status = WL_IDLE_STATUS;
    int* trackerStatus;
    LinearActuator* actuator;
    WindSpeedSensor* windSensor;
    ClockModule* clockModule;
    WiFiServer server;
    static const String trackerStatusStrings[];
    void endConnection(WiFiClient client);
    void getParams(String params);
    void attemptConnection(char*, char*);
    void attemptCreation();
    void sendDashboardTo(WiFiClient client);
    void actOnParameter(JsonDocument params);
};

#endif