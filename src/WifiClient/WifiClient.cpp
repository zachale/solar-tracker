#include "WifiClient.h"
#include "../SettingsManager/SettingsManager.h"

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

  // Soft reset the radio before associating. An access point listening socket
  // survives the switch into station mode and keeps its socket, and the ESP32 has
  // few enough of them that one held listener is the difference between a request
  // that works and one that cannot connect at all. Measured on this hardware:
  // without this the clock sync succeeds and the upload immediately after it fails
  // with a connection refused. WiFi.end() is the only call that reclaims them.
  WiFi.end();

  auto credentials = SettingsManager::getWifiCredentials();
  for (const auto &credential : credentials)
  {
    WiFi.begin(credential.first.c_str(), credential.second.c_str());
    Serial.print(F("Connecting to WiFi"));
    Serial.println(credential.first);
    for (int i = 0; i < 5; i++)
    {
      delay(1000);
      Serial.print(F("."));
      if (WiFi.status() == WL_CONNECTED)
      {
        Serial.println("Successfully connected to" + String(credential.first));
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
  // Deliberately no "Connection: close". R4HttpClient reads the response inside
  // a `while (client->connected() ...)` loop, so a server that hangs up as soon
  // as it has replied ends the read part-way through the headers and the body is
  // discarded. Leaving the connection alive lets the whole response be read.

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

// Returns the HTTP status code, or 0 when the request never got off the board.
// Deliberately a single attempt: the body can be several kilobytes, and anything
// that fails here is retried at the next sync without losing data, because the
// caller only clears the spool on a 200.
int WifiClient::post(String url, const String &body, const char *contentType)
{
  if (!ensureWifiConnection())
  {
    return 0;
  }
  url = ensureHTTPS(url);
  Serial.println("POST Request");
  Serial.println(url);
  http.begin(client, url, 443);
  http.setTimeout(15000);
  http.addHeader("User-Agent: Arduino UNO R4 WiFi");
  http.addHeader(String("Content-Type: ") + contentType);

  int responseNum = http.POST(body);
  Serial.println("Response code: " + String(responseNum));
  http.close();
  return responseNum;
}