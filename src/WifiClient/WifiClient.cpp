#include "WifiClient.h"
#include "../../secrets.h"

void WifiClient::setup()
{
  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION)
    Serial.println(F("Please upgrade the firmware"));

  if (WiFi.status() == WL_NO_MODULE)
  {
    Serial.println(F("Communication with WiFi module failed!"));
    while (true)
      ;
  }

  for (const auto &credential : wifiCredentials)
  {
    WiFi.begin(credential.first, credential.second);
    Serial.print(F("Connecting to WiFi"));
    Serial.println(credential.first);
    for (int i = 0; i < 5 && WiFi.status() != WL_CONNECTED; i++)
    {
      delay(1000);
      Serial.print(F("."));
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      break;
    }
  }
  Serial.println();
  Serial.println(F("Successfully connected to WiFi!"));
}

String WifiClient::get(String url)
{
  Serial.println("GET Request");
  Serial.println(url);
  http.begin(client, url, 443);
  http.setTimeout(15000);
  // http.addHeader("User-Agent: Arduino UNO R4 WiFi");
  http.addHeader("Content-Type: application/json");
  // http.addHeader("Connection: close");

  int responseNum = http.GET();
  if (responseNum > 0)
  {
    String responseBody = http.getBody();
    Serial.println(responseBody);
    Serial.println("Response code: " + String(responseNum));
    http.close();
    return responseBody;
  }
  String responseBody = http.getBody();
  Serial.println(responseBody);
  Serial.println("Request Failed: " + String(responseNum));
  http.close();
  return "";
}

void WifiClient::post(String url, JsonDocument doc)
{
  // TODO: Add post request functionality
}