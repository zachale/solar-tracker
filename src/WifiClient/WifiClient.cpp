#include "WifiClient.h"

void WifiClient::setup()
{
  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION)
    Serial.println(F("Please upgrade the firmware"));

  if (WiFi.status() == WL_NO_MODULE)
  {
    Serial.println(F("Communication with WiFi module failed!"));
    while (true);
  }

  WiFi.begin(SSID, PASS);
  Serial.print(F("Connecting to WiFi"));
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.print(F("."));
  }
  Serial.println();
  Serial.println(F("Successfully connected to WiFi!"));
}

void WifiClient::get(String url)
{
  Serial.println("GET Request");
  Serial.println(url);  
  http.begin(client, url, 443);
  http.setTimeout(3000);
  http.addHeader("User-Agent: Arduino UNO R4 WiFi");
  http.addHeader("Connection: close");

  int responseNum = http.GET();
  if (responseNum > 0)
  {
    String responseBody = http.getBody();
    Serial.println(responseBody);
    Serial.println("Response code: " + String(responseNum));
  } else {
    Serial.println("Request Failed: " + String(responseNum));
  }

  http.close();
}

void WifiClient::post(String url, JsonDocument doc)
{
  String requestBody;
  serializeJson(doc, requestBody);

  http.begin(client, "https://example.org", 443);
  http.setTimeout(3000);
  http.addHeader("User-Agent: Arduino UNO R4 WiFi");
  http.addHeader("Content-Type: application/json");

  int responseNum = http.POST(requestBody);
  if (responseNum > 0)
  {
    String responseBody = http.getBody();
    Serial.println(responseBody);
    Serial.println("Response code: " + String(responseNum));
  } else {
    Serial.println("Request Failed: " + String(responseNum));
  }

  http.close();
}