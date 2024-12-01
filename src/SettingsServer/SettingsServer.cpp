#include "./SettingsServer.h"

SettingsServer::SettingsServer(SolarTracker *inputTracker) : server(80)
{
  tracker = inputTracker;
}

void SettingsServer::setup()
{
  // Initialize serial and wait for port to open:
  Serial.println("Access Point Web Server");

  // check for the WiFi module:
  if (WiFi.status() == WL_NO_MODULE)
  {
    Serial.println("Communication with WiFi module failed!");
    // don't continue
    while (true)
      ;
  }

  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION)
  {
    Serial.println("Please upgrade the firmware");
  }

  attemptCreation();
  // wait 10 seconds for connection:
  delay(10000);

  // start the web server on port 80
  server.begin();

  // you're connected now, so print out the status
  printWiFiStatus();
}

void SettingsServer::attemptCreation()
{
  // print the network name (SSID);
  Serial.print("Creating access point named: ");
  Serial.println(ssid);

  // Create open network. Change this line if you want to create an WEP network:
  status = WiFi.beginAP(ssid);
  if (status != WL_AP_LISTENING)
  {
    Serial.println("Creating access point failed");
    while (true)
      ;
  }
}

void SettingsServer::checkForClient()
{

  bool isPost = false;

  // compare the previous status to the current status
  if (status != WiFi.status())
  {
    // it has changed update the variable
    status = WiFi.status();

    if (status == WL_AP_CONNECTED)
    {
      // a device has connected to the AP
      Serial.println("Device connected to AP");
    }
    else
    {
      // a device has disconnected from the AP, and we are back in listening mode
      Serial.println("Device disconnected from AP");
    }
  }

  WiFiClient client = server.available(); // listen for incoming clients

  if (client)
  {                               // if you get a client,
    Serial.println("new client"); // print a message out the serial port
    String currentLine = "";      // make a String to hold incoming data from the client
    int newLineCount = 0;
    while (client.connected())
    { // loop while the client's connected
      if (client.available())
      {                         // if there's bytes to read from the client,
        char c = client.read(); // read a byte, then
        // Serial.write(c);                    // print it out to the serial monitor
        if (c == '\n')
        { // if the byte is a newline character
          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:
          newLineCount++;

          if (currentLine.length() == 0 && newLineCount > 5)
          {

            if (isPost)
            {
              getParams(client.readString());
            }

            sendDashboardTo(client);

            // break out of the while loop:
            break;
          }
          else
          { // if you got a newline, then clear currentLine:
            currentLine = "";
          }
        }
        else if (c != '\r')
        {                   // if you got anything else but a carriage return character,
          currentLine += c; // add it to the end of the currentLine
        }

        if (currentLine.startsWith("POST"))
        {
          isPost = true;
        }

        // Check to see if the client request was "GET /H" or "GET /L":
        if (currentLine.endsWith("GET /recalibrate"))
        {
          tracker->actuator.recalibrate();
          endConnection(client);
          return;
        }
      }
    }

    endConnection(client);
  }
}

void SettingsServer::getParams(String params)
{
  int index = params.indexOf("&");
  bool parseMultiParams = index == -1 ? false : true;
  JsonDocument docParams;
  std::vector<char *> collection;
  char buf[params.length() + 1];
  params.toCharArray(buf, sizeof(buf));
  if (parseMultiParams)
  {
    char *p = buf;
    char *str;
    // loop until strtok() returns NULL
    while ((str = strtok_r(p, "&", &p)) != NULL)
    { // delimiter is the semicolon
      collection.push_back(str);
    }
  }
  else
  {
    collection.push_back(buf);
  }
  Serial.println(collection.size());
  for (int i = 0; i < collection.size(); i++)
  {
    String title = strtok(collection[i], "=");
    String description = strtok(NULL, "");
    docParams[title] = description;
  }
  actOnParameter(docParams);
}

void SettingsServer::actOnParameter(JsonDocument params)
{

  Serial.println("Acting on param");
  String output;
  serializeJson(params, output);
  Serial.println(output);

  if (params["status"])
  {
    tracker->setStatus(params["status"].as<SolarTracker::Status>());
  }

  if (params["recalibrateActuator"])
  {
    tracker->actuator.recalibrate();
  }

  if (params["extendToPercent"])
  {
    int extendTo = params["extendToPercent"].as<int>();
    tracker->actuator.extendToPercent(extendTo);
  }

  if (params["windUpperMaxSpeed"])
  {
    float windUpperMax = params["windUpperMaxSpeed"].as<float>();
    tracker->windSensor.setUpperSpeedMax(windUpperMax / 3.6);
  }

  if (params["windUpperWait"])
  {
    float windUpperWait = params["windUpperWait"].as<float>();
    tracker->windSensor.setUpperSpeedDelay(windUpperWait * 6000);
  }

  if (params["windLowerSpeedMax"])
  {
    float windLowerMax = params["windLowerSpeedMax"].as<float>();
    tracker->windSensor.setLowerSpeedMax(windLowerMax / 3.6);
  }

  if (params["windLowerWait"])
  {
    int windLowerWait = params["windLowerWait"].as<float>();
    tracker->windSensor.setLowerSpeedDelay(windLowerWait * 6000);
  }

  if (params["setTime"].is<String>())
  {
    String timeStr = params["setTime"];
    char *delimiter = "%3A"; // This delimiter is the character ':' url encoded
    int index = timeStr.indexOf(delimiter);
    Serial.println(timeStr);
    Serial.println(index);
    int hour = timeStr.substring(0, index).toInt();
    int minute = timeStr.substring(index + 3).toInt();
    tracker->clockModule.setSimpleTime(hour, minute);
  }

  if (params["setStartHour"])
  {
    tracker->clockModule.setSchedule(params);
  }
}

void SettingsServer::endConnection(WiFiClient client)
{
  client.stop();
  Serial.println("client disconnected");
}

void SettingsServer::printWiFiStatus()
{
  // print the SSID of the network you're attached to:
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print your WiFi shield's IP address:
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  // print where to go in a browser:
  Serial.print("To see this page in action, open a browser to http://");
  Serial.println(ip);
}

// Prints dashboard HTML to connected client
void SettingsServer::sendDashboardTo(WiFiClient client)
{
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/html");
  client.println();
  client.print(R"(<!DOCTYPE HTML><html><head><title>Solar Tracker Dashboard</title><style>)");
  client.print(R"(body {padding: 10px;} section {padding-left: 2rem;padding-right: 2rem;margin-bottom: 4px;font-size:5vw;} input {margin-bottom:8px;})");
  client.print(R"(</style></head><body>)");

  client.print(R"(<h1 style="font-size:10vw;">Dashboard</h1>)");

  client.print(R"(<section><p style="font-size:5vw;">)");
  client.print("Wind speed: ");
  client.print(tracker->windSensor.getSpeed()); // Placeholder: wind speed from sensor
  client.print("<br/>");

  client.print("Extended to: ");
  client.print(tracker->actuator.getPercentExtended()); // Placeholder: percent extended from actuator
  client.print("%<br/>");

  client.print("Time: ");
  client.print(tracker->clockModule.getFullTimeString()); // Placeholder: current time from clock module
  client.print("<br/>");

  client.print("Status: ");
  client.print(tracker->getStatusString()); // Placeholder: tracker status, e.g., HIGH_WIND,
  client.print(R"(</p></section>)");

  client.print(R"(<section><form action="/" method="POST">)");
  client.print(R"(<button type="submit" style="font-size:5vw;" name="status" value="0">Set Active</button>)");
  client.print(R"(<button type="submit" style="font-size:5vw;" name="status" value="2">Set Safe</button>)");
  client.print(R"(<button type="submit" style="font-size:5vw;" name="status" value="3">Set Away</button>)");
  client.print(R"(</form></section>)");

  client.print(R"(<h1 style="font-size:7vw;">Actuator</h1>)");

  client.print(R"(<section><form action="/" method="POST">)");
  client.print(R"(<button type="submit" style="font-size:5vw;" name="recalibrateActuator" value="true">Recalibrate</button>)");
  client.print(R"(</form></section>)");

  client.print(R"(<section><form action="/" method="POST">)");
  client.print("Extend To ");
  client.print(R"(<input style="font-size:5vw;" type="number" max="100" min="0" name="extendToPercent" value=")");
  client.print(tracker->actuator.getPercentExtended());
  client.print(R"("> <input type="submit" style="font-size:5vw;" value="Submit">)");
  client.print(R"(</form></section>)");

  client.print(tracker->windSensor.toHtml());
  client.print(tracker->clockModule.toHtml());

  // The HTTP response ends with another blank line:
  client.println();
}