#include "./ConfigManager.h"

// Initialize the SD card
bool ConfigManager::begin(uint8_t csPin)
{
  return SD.begin(csPin);
}

// Save settings to file
bool ConfigManager::saveSettings()
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
bool ConfigManager::loadSettings()
{
  File configFile = SD.open(storageFile, FILE_READ);
  if (!configFile)
  {
    Serial.println("error opening config file");
    return false;
  }
  DeserializationError error = deserializeJson(configDocument, configFile);
  configFile.close();
  return !error;
}
