#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>

class SettingsManager
{
private:
  static StaticJsonDocument<1024> configDocument;
  static constexpr const char *storageFile = "config";

public:
  static bool begin(uint8_t csPin);

  template <typename T>
  static void upsertSetting(char *key, T value)
  {
    configDocument[key] = value;
  }

  static bool save();

  static bool load();

  template <typename T>
  static T getSetting(char *key)
  {
    return configDocument[key].as<T>();
  }
};
#endif