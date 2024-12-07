#ifndef WC_H
#define WC_H

#include <Arduino.h>
#include <R4HttpClient.h>
#include "WiFiS3.h"
#include <ArduinoJson.h>

class WifiClient
{
public:
  static bool ensureWifiConnection();
  String get(String);
  void post(String, JsonDocument);

private:
  WiFiSSLClient client;
  R4HttpClient http;
  static std::pair<const char *, const char *> savedCredentials;
  String ensureHTTPS(String);
};

#endif