#ifndef WC_H
#define WC_H

#include <Arduino.h>
#include <R4HttpClient.h>
#include "WiFiS3.h"
#include <ArduinoJson.h>

class WifiClient
{
public:
  void setup();
  void get(String);
  void post(String, JsonDocument);  
private:
  WiFiSSLClient client;
  R4HttpClient http;
};

#endif