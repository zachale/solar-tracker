#include "./Wifi.h"

WifiModule::WifiModule(int* inputStatus, LinearActuator* inputActuator, WindSpeedSensor* inputWindSensor, ClockModule*  inputClock) : server(80){
  actuator = inputActuator;
  windSensor = inputWindSensor;
  clockModule = inputClock;
  trackerStatus = inputStatus;
}

const String WifiModule::trackerStatusStrings[] =  {"Active",  "Night Mode", "Safe Mode", "Away Mode"};


void WifiModule::setup() {
  //Initialize serial and wait for port to open:
  Serial.println("Access Point Web Server");

  // check for the WiFi module:
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed!");
    // don't continue
    while (true);
  }

  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.println("Please upgrade the firmware");
  }

  attemptConnection(DEVSSID, DEVPASS);
  attemptCreation();
  // wait 10 seconds for connection:
  delay(10000);

  // start the web server on port 80
  server.begin();

  // you're connected now, so print out the status
  printWiFiStatus();
}

void WifiModule::attemptConnection(char* ssid, char* pass){
  if (status != WL_CONNECTED) {
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(ssid);
  // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
    status = WiFi.begin(ssid, pass);
    delay(3000);
  }
}

void WifiModule::attemptCreation(){
    // print the network name (SSID);
    Serial.print("Creating access point named: ");
    Serial.println(ssid);

    // Create open network. Change this line if you want to create an WEP network:
    status = WiFi.beginAP(ssid);
    if (status != WL_AP_LISTENING) {
      Serial.println("Creating access point failed");
      while(true);
    }
}


void WifiModule::checkForClient() {

  bool isPost = false;
  
  // compare the previous status to the current status
  if (status != WiFi.status()) {
    // it has changed update the variable
    status = WiFi.status();

    if (status == WL_AP_CONNECTED) {
      // a device has connected to the AP
      Serial.println("Device connected to AP");
    } else {
      // a device has disconnected from the AP, and we are back in listening mode
      Serial.println("Device disconnected from AP");
    }
  }
  
  WiFiClient client = server.available();   // listen for incoming clients

  if (client) {                             // if you get a client,
    Serial.println("new client");           // print a message out the serial port
    String currentLine = "";                // make a String to hold incoming data from the client
    int newLineCount = 0;
    while (client.connected()) {            // loop while the client's connected
      if (client.available()) {             // if there's bytes to read from the client,
        char c = client.read();             // read a byte, then
        // Serial.write(c);                    // print it out to the serial monitor
        if (c == '\n') {                    // if the byte is a newline character
          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:
          newLineCount++;

          if (currentLine.length() == 0 && newLineCount > 5) {

            if(isPost){
              getParams(client.readString());
            }
      
            sendDashboardTo(client);

            // break out of the while loop:
            break;
          }
          else {      // if you got a newline, then clear currentLine:
            currentLine = "";
          }
        }
        else if (c != '\r') {    // if you got anything else but a carriage return character,
          currentLine += c;      // add it to the end of the currentLine
        }

        if (currentLine.startsWith("POST")) {
          isPost = true;
        }

        // Check to see if the client request was "GET /H" or "GET /L":       
        if (currentLine.endsWith("GET /recalibrate")) {
          actuator->recalibrate();
          endConnection(client);  
          return;              
        }
      }   
    }
    
    endConnection(client);
  }
}

void WifiModule::getParams(String params){
  int index = params.indexOf("&");
  bool parseMultiParams = index == -1 ? false : true;
  JsonDocument docParams;
  std::vector<char*> collection;
  char buf[params.length()+1];
  params.toCharArray(buf, sizeof(buf));
  if(parseMultiParams){
    char *p = buf;
    char *str;
    // loop until strtok() returns NULL
    while ((str = strtok_r(p, "&", &p)) != NULL){ // delimiter is the semicolon
      collection.push_back(str);
    }
  } else {
    collection.push_back(buf);
  }
  Serial.println(collection.size());
  for(int i = 0; i < collection.size(); i++){
    String title = strtok(collection[i], "=");
    String description = strtok(NULL,"");
    docParams[title] = description;
  }
  actOnParameter(docParams);
}

void WifiModule::actOnParameter(JsonDocument params){

  Serial.println("Acting on param");
  String output;
  serializeJson(params, output);  
  Serial.println(output);


  if (params["status"]) {
    *trackerStatus = params["status"].as<int>();
  }

  if (params["recalibrateActuator"]) {
    actuator->recalibrate();
  }

  if (params["extendToPercent"]) {
    int extendTo = params["extendToPercent"].as<int>();
    actuator->extendToPercent(extendTo);
  }

  if (params["windUpperMaxSpeed"]) {
    float windUpperMax = params["windUpperMaxSpeed"].as<float>();
    windSensor->setUpperSpeedMax(windUpperMax / 3.6);
  }

  if (params["windUpperWait"]) {
    float windUpperWait = params["windUpperWait"].as<float>();
    windSensor->setUpperSpeedDelay(windUpperWait / 3.6);
  }

  if (params["windLowerSpeedMax"]) {
    float windLowerMax = params["windLowerSpeedMax"].as<float>();
    windSensor->setLowerSpeedMax(windLowerMax);
  }

  if (params["windLowerWait"]) {
    int windLowerWait = params["windLowerWait"].as<float>();
    windSensor->setLowerSpeedDelay(windLowerWait);
  }

  if (params["setTime"].is<String>()) {
    String timeStr = params["setTime"];
    char* delimiter = "%3A"; // This delimiter is the character ':' url encoded
    int index = timeStr.indexOf(delimiter);
    Serial.println(timeStr);
    Serial.println(index);
    int hour = timeStr.substring(0, index).toInt();
    int minute = timeStr.substring(index + 3).toInt();
    clockModule->setSimpleTime(hour, minute);
  }

  if (params["recalibrateTime"].is<bool>()) {
    clockModule->wifiRecalibrate();
  }
}

void WifiModule::endConnection(WiFiClient client){
  client.stop();
  Serial.println("client disconnected");
}

void WifiModule::printWiFiStatus() {
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
void WifiModule::sendDashboardTo(WiFiClient client){
  client.println("HTTP/1.1 200 OK");
  client.println("Content-type:text/html");
  client.println();
  client.print(R"(<!DOCTYPE HTML><html><head><title>Solar Tracker Dashboard</title><style>)");
  client.print(R"(body {padding: 10px;} section {padding-left: 2rem;padding-right: 2rem;margin-bottom: 4px;font-size:5vw;} input {margin-bottom:8px;})");
  client.print(R"(</style></head><body>)");

  client.print(R"(<h1 style="font-size:10vw;">Dashboard</h1>)");

  client.print(R"(<section><p style="font-size:5vw;">)");
  client.print("Wind speed: ");
  client.print(windSensor->getSpeed());  // Placeholder: wind speed from sensor
  client.print("<br/>");

  client.print("Extended to: ");
  client.print(actuator->getPercentExtended());  // Placeholder: percent extended from actuator
  client.print("%<br/>");

  client.print("Time: ");
  client.print(clockModule->getFullTimeString());  // Placeholder: current time from clock module
  client.print("<br/>");

  client.print("Status: ");
  client.print(trackerStatusStrings[*trackerStatus]);  // Placeholder: tracker status, e.g., HIGH_WIND, ACTIVE, etc.
  client.print(R"(</p></section>)");

  client.print(R"(<section><form action="/" method="POST">)");
  client.print(R"(<button type="submit" style="font-size:5vw;" name="status" value="ACTIVE">Set Active</button>)");
  client.print(R"(<button type="submit" style="font-size:5vw;" name="status" value="SAFE">Set Safe</button>)");
  client.print(R"(<button type="submit" style="font-size:5vw;" name="status" value="AWAY">Set Away</button>)");
  client.print(R"(</form></section>)");

  client.print(R"(<h1 style="font-size:7vw;">Actuator</h1>)");

  client.print(R"(<section><form action="/" method="POST">)");
  client.print(R"(<button type="submit" style="font-size:5vw;" name="recalibrateActuator" value="true">Recalibrate</button>)");
  client.print(R"(</form></section>)");

  client.print(R"(<section><form action="/" method="POST">)");
  client.print("Extend To ");
  client.print(R"(<input style="font-size:5vw;" type="number" max="100" min="0" name="extendToPercent" value=")");
  client.print(actuator->getPercentExtended());
  client.print(R"("> <input type="submit" style="font-size:5vw;" value="Submit">)");
  client.print(R"(</form></section>)");

  client.print(R"(<h1 style="font-size:7vw;">Wind Sensor</h1>)");

  client.print(R"(<section><form action="/" method="POST">)");
  client.print("Set upper threshold wind speed max (m/s): ");
  client.print(R"(<input style="font-size:5vw;" type="number" max="32" min="0" name="windUpperMaxSpeed" value=")");
  client.print(windSensor->getUpperSpeedMax() * 3.6); // lower wind threshold speed m/s converted to km/h
  client.print(R"("> <input type="submit" style="font-size:5vw;" value="Submit">)");
  client.print(R"(</form></section>)");

  client.print(R"(<section><form action="/" method="POST">)");
  client.print("Set upper threshold wait period (seconds): ");
  client.print(R"(<input style="font-size:5vw;" type="number" max="32" min="0" name="windUpperWait" value=")");
  client.print(windSensor->getUpperSpeedDelay() / 6000); // upper wind threshold wait period miliseconds converted to minutes
  client.print(R"("> <input type="submit" style="font-size:5vw;" value="Submit">)");
  client.print(R"(</form></section>)");

  client.print(R"(<section><form action="/" method="POST">)");
  client.print("Set lower threshold wind speed max (m/s): ");
  client.print(R"(<input style="font-size:5vw;" type="number" max="100" min="0" name="windLowerSpeedMax" value=")");
  client.print(windSensor->getLowerSpeedMax() * 3.6); // lower wind threshold speed m/s converted to km/h
  client.print(R"("> <input type="submit" style="font-size:5vw;" value="Submit">)");
  client.print(R"(</form></section>)");

  client.print(R"(<section><form action="/" method="POST">)");
  client.print("Set lower threshold wait period (seconds): ");
  client.print(R"(<input style="font-size:5vw;" type="number" max="100" min="0" name="windLowerWait" value=")");
  client.print(windSensor->getLowerSpeedDelay() / 6000); // lower wind threshold wait period miliseconds converted to minutes
  client.print(R"("> <input type="submit" style="font-size:5vw;" value="Submit">)");
  client.print(R"(</form></section>)");

  client.print(R"(<h1 style="font-size:7vw;">Clock</h1>)");

  client.print(R"(<section><form action="/" method="POST">)");
  client.print("Set time (HH:MM - 24 hour format): ");
  client.print(R"(<input style="font-size:5vw;" type="text" pattern="^([01]\d|2[0-3]):([0-5]\d)$" name="setTime" value=")");
  client.print(clockModule->getSimpleTimeString());
  client.print(R"("> <input type="submit" style="font-size:5vw;" value="Submit">)");
  client.print(R"(</form></section>)");

  client.print(R"(<section><form action="/" method="POST">)");
  client.print(R"(<button type="submit" style="font-size:5vw;" name="recalibrateTime" value="true">Recalibrate using internet</button>)");
  client.print(R"(</form></section>)");

  client.print("Timestamp: ");
  client.print(clockModule->getTimestamp());
  client.print(R"(</body></html>)");
  
  // The HTTP response ends with another blank line:
  client.println();
}