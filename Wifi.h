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
    WifiModule(LinearActuator*, WindSpeedSensor*, ClockModule*);
    void setup();
    void checkForClient();
    void printWiFiStatus();
  private:
    const char *ssid = "Solar-Tracker";
    int keyIndex = 0;                 
    int status = WL_IDLE_STATUS;
    LinearActuator* actuator;
    WindSpeedSensor* windSensor;
    ClockModule* clockModule;
    WiFiServer server;
    void endConnection(WiFiClient client);
    void updateParams(String params);
    void attemptConnection(char*, char*);
    void attemptCreation();
};

#endif