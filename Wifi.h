#ifndef W_H
#define W_H

#include "WiFiS3.h"
#include "./LinearActuator.h" 
#include "./WindSensor.h" 
#include <ArduinoJson.h>
#include "./secrets.h"


class WifiModule {
  public:
    JsonDocument lastParams;
    WifiModule(LinearActuator, WindSensor);
    void setup();
    void checkForClient();
    void printWiFiStatus();
  private:
    const char *ssid = "Solar-Tracker";
    int keyIndex = 0;                 
    int status = WL_IDLE_STATUS;
    LinearActuator actuator;
    WindSensor windSensor;
    WiFiServer server;
    void endConnection(WiFiClient client);
    void updateParams(String params);
    void attemptConnection(char*, char*);
    void attemptCreation();
};

#endif