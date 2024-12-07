#ifndef OTA_UPDATER_H
#define OTA_UPDATER_H

#include <Arduino.h>
#include "WiFiS3.h"
#include <ArduinoHttpClient.h>
#include "../WifiClient/WifiClient.h"

#define NO_OTA_NETWORK
#include <ArduinoOTA.h> // only for InternalStorage

#include "../../secrets.h"

class OTAUpdater
{
public:
  bool updateAvailable();
  void update();

private:
  WiFiSSLClient wifiClientSSL;
  WifiClient wifiClient;
  const short VERSION = 1;
};

#endif