
#include "./OTAUpdater.h"

bool OTAUpdater::updateAvailable()
{
  if (!WifiClient::ensureWifiConnection())
  {
    return false;
  };
  String requestURL = "";
  requestURL.concat(API);
  requestURL.concat("/update/check/");
  requestURL.concat(VERSION + 1);
  String body = wifiClient.get(requestURL);
  if (body != "")
  {
    StaticJsonDocument<200> doc;
    deserializeJson(doc, body);
    return doc["updateAvailable"].as<bool>();
  }
  return false;
}

void OTAUpdater::update()
{
  if (!WifiClient::ensureWifiConnection())
  {
    return;
  };
  const unsigned short SERVER_PORT = 443;               // Commonly 80 (HTTP) | 443 (HTTPS)
  const char *PATH = "/update/download/update-v%d.bin"; // Set the URI to the .bin firmware

  HttpClient client(wifiClientSSL, API, SERVER_PORT); // HTTPS
  char buff[32];
  snprintf(buff, sizeof(buff), PATH, VERSION + 1);

  Serial.print("Check for update file ");
  Serial.println(buff);

  // Make the GET request
  client.get(buff);

  int statusCode = client.responseStatusCode();
  Serial.print("Update status code: ");
  Serial.println(statusCode);
  if (statusCode != 200)
  {
    client.stop();
    return;
  }

  long length = client.contentLength();
  if (length == HttpClient::kNoContentLengthHeader)
  {
    client.stop();
    Serial.println("Server didn't provide Content-length header. Can't continue with update.");
    return;
  }
  Serial.print("Server returned update file of size ");
  Serial.print(length);
  Serial.println(" bytes");

  if (!InternalStorage.open(length))
  {
    client.stop();
    Serial.println("There is not enough space to store the update. Can't continue with update.");
    return;
  }
  byte b;
  while (length > 0)
  {
    if (!client.readBytes(&b, 1)) // reading a byte with timeout
      break;
    InternalStorage.write(b);
    length--;
  }
  InternalStorage.close();
  client.stop();
  if (length > 0)
  {
    Serial.print("Timeout downloading update file at ");
    Serial.print(length);
    Serial.println(" bytes. Can't continue with update.");
    return;
  }

  Serial.println("Sketch update apply and reset.");
  Serial.flush();
  InternalStorage.apply(); // this doesn't return
}