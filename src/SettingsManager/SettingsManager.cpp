#include "./SettingsManager.h"

StaticJsonDocument<1024> SettingsManager::configDocument;
constexpr const char *SettingsManager::storageFile; // Define storageFile
std::vector<std::pair<String, String>> SettingsManager::wifiCredentials;
constexpr const char *SettingsManager::wifiStorageFile;

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
  return saveWifiCredentials();
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
  loadWifiCredentials();
  return !error;
}

bool SettingsManager::saveWifiCredentials()
{
  SD.remove(wifiStorageFile);
  File wifiFile = SD.open(wifiStorageFile, FILE_WRITE);
  if (!wifiFile)
  {
    Serial.println("error opening wifi credentials file for writing");
    return false;
  }
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (const auto &cred : wifiCredentials)
  {
    JsonObject obj = array.add<JsonObject>();
    obj["ssid"] = cred.first;
    obj["pass"] = cred.second;
  }
  serializeJson(doc, wifiFile);
  wifiFile.close();
  return true;
}

bool SettingsManager::loadWifiCredentials()
{
  File wifiFile = SD.open(wifiStorageFile, FILE_READ);
  if (!wifiFile)
  {
    Serial.println("error opening wifi credentials file for reading");
    return false;
  }
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, wifiFile);
  if (error)
  {
    Serial.println("Failed to read wifi credentials file, using default");
    wifiFile.close();
    return false;
  }

  wifiCredentials.clear();
  JsonArray array = doc.as<JsonArray>();
  for (JsonObject obj : array)
  {
    wifiCredentials.push_back({obj["ssid"].as<String>(), obj["pass"].as<String>()});
  }
  wifiFile.close();
  return true;
}

void SettingsManager::addWifiCredential(String ssid, String pass)
{
  wifiCredentials.push_back({ssid, pass});
}

void SettingsManager::removeWifiCredential(int index)
{
  if (index >= 0 && index < wifiCredentials.size())
  {
    wifiCredentials.erase(wifiCredentials.begin() + index);
  }
}

std::vector<std::pair<String, String>> SettingsManager::getWifiCredentials()
{
  return wifiCredentials;
}
