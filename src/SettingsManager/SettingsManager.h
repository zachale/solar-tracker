#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>
#include <vector>

class SettingsManager
{
private:
  static StaticJsonDocument<1024> configDocument;
  static constexpr const char *storageFile = "config";
  static constexpr const char *wifiStorageFile = "wifi_credentials.json";

public:
  static std::vector<std::pair<String, String>> wifiCredentials;
  static bool begin(uint8_t csPin);

  template <typename T>
  static void upsertSetting(char *key, T value)
  {
    configDocument[key] = value;
  }

  static bool save();
  static bool load();

  static bool saveWifiCredentials();
  static bool loadWifiCredentials();
  static void addWifiCredential(String ssid, String pass);
  static void removeWifiCredential(int index);
  static std::vector<std::pair<String, String>> getWifiCredentials();

  template <typename T>
  static T getSetting(char *key)
  {
    return configDocument[key].as<T>();
  }

  static String getConfigDoc()
  {
    return configDocument.as<String>();
  }
};
#endif