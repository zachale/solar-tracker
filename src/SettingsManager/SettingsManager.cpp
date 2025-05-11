#include "./SettingsManager.h"

StaticJsonDocument<1024> SettingsManager::configDocument;
constexpr const char *SettingsManager::storageFile; // Define storageFile

// Initialize the SD card
bool SettingsManager::begin(uint8_t csPin)
{
  return SD.begin(csPin);
}

// Save settings to file
bool SettingsManager::save()
{
  SD.remove(storageFile);
  File configFile = SD.open(storageFile, FILE_WRITE);
  if (!configFile)
  {
    Serial.println("error opening config file");
    return false;
  }
  configFile.seek(0);
  serializeJson(configDocument, configFile);
  configFile.close();
  return true;
}

// Load settings from file
bool SettingsManager::load()
{
  File configFile = SD.open(storageFile, FILE_READ);
  if (!configFile)
  {
    return false;
  }
  DeserializationError error = deserializeJson(configDocument, configFile);
  configFile.close();
  return !error;
}
