#include "WifiClient.h"
#include "../../secrets.h"

std::pair<const char *, const char *> WifiClient::savedCredentials = {"Default", "Default"};

bool WifiClient::ensureWifiConnection()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return true;
  }
  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION)
    Serial.println(F("Please upgrade the firmware"));

  if (WiFi.status() == WL_NO_MODULE)
  {
    Serial.println(F("Communication with WiFi module failed!"));
    return false;
  }
  if (strcmp(WifiClient::savedCredentials.first, WifiClient::savedCredentials.second) != 0)
  {
    WiFi.begin(WifiClient::savedCredentials.first, WifiClient::savedCredentials.second);
    Serial.println(F("Reconnecting to WiFi"));
    for (int i = 0; i < 5; i++)
    {
      delay(1000);
      Serial.print(F("."));
    }
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println(F("Successfully reconnected to WiFi!"));
      return true;
    }
  }
  for (const auto &credential : wifiCredentials)
  {
    WiFi.begin(credential.first, credential.second);
    Serial.print(F("Connecting to WiFi"));
    Serial.println(credential.first);
    for (int i = 0; i < 5; i++)
    {
      delay(1000);
      Serial.print(F("."));
      if (WiFi.status() == WL_CONNECTED)
      {
        Serial.println("Successfully connected to" + String(credential.first));
        WifiClient::savedCredentials = credential;
        return true;
      }
    }
  }
  Serial.println(F("Failed to connect to WiFi!"));
  return false;
}

String WifiClient::get(String url)
{
  if (!ensureWifiConnection())
  {
    return "";
  }
  Serial.println("GET Request");
  url = ensureHTTPS(url);
  Serial.println(url);
  http.begin(client, url, 443);
  http.setTimeout(15000);
  http.addHeader("User-Agent: Arduino UNO R4 WiFi");
  http.addHeader("Content-Type: application/json");
  http.addHeader("Connection: close");

  int responseNum;

  for (int i = 0; i < 10; i++)
  {
    responseNum = http.GET();
    delay(100);
    if (responseNum > 0)
    {
      String responseBody = http.getBody();
      if (responseBody != "")
      {
        Serial.println("Response code: " + String(responseNum));
        Serial.println("It took " + String(i + 1) + " attempts to get a response");
        Serial.println(responseBody);
        http.close();
        return responseBody;
      }
    }
  }

  String responseBody = http.getBody();
  Serial.println(responseBody);
  Serial.println("Request Failed: " + String(responseNum));
  http.close();
  WiFi.disconnect();
  return "";
}

String WifiClient::ensureHTTPS(String url)
{
  if (url.startsWith("https://"))
  {
    return url;
  }
  return "https://" + url;
}

void WifiClient::post(String url, JsonDocument doc)
{
  // TODO: Add post request functionality
}