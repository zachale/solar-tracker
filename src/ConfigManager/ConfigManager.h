#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>

class ConfigManager
{
private:
  StaticJsonDocument<1024> configDocument; // Adjust size based on needs
  const char *storageFile = "config";      // File for saving settings
public:
  bool begin(uint8_t csPin);
  // Register settings from sensor class

  template <typename T>
  void registerSettings(char *key, T value)
  {
    configDocument[key] = value;
  }
  // Save settings to file
  bool saveSettings();
  // Load settings from file
  bool loadSettings();
  // Get a specific setting
  template <typename T>
  T getSetting(char *key)
  {
    return configDocument[key].as<T>();
  }
};
#endif