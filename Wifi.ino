#include "./Wifi.h"

WifiModule::WifiModule(LinearActuator inputActuator, WindSensor inputWindSensor) : server(80){
  actuator = inputActuator;
  windSensor = inputWindSensor;
}


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

  // by default the local IP address will be 192.168.4.1
  // you can override it with the following:
  WiFi.config(IPAddress(192,48,56,2));

  // print the network name (SSID);
  Serial.print("Creating access point named: ");
  Serial.println(ssid);

  // Create open network. Change this line if you want to create an WEP network:
  status = WiFi.beginAP(ssid);
  if (status != WL_AP_LISTENING) {
    Serial.println("Creating access point failed");
    // don't continue
    while (true);
  }

  // wait 10 seconds for connection:
  delay(10000);

  // start the web server on port 80
  server.begin();

  // you're connected now, so print out the status
  printWiFiStatus();
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
      delayMicroseconds(10);                // This is required for the Arduino Nano RP2040 Connect - otherwise it will loop so fast that SPI will never be served.
      if (client.available()) {             // if there's bytes to read from the client,
        char c = client.read();             // read a byte, then
        // Serial.write(c);                    // print it out to the serial monitor
        if (c == '\n') {                    // if the byte is a newline character
          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:
          newLineCount++;

          if (currentLine.length() == 0 && newLineCount > 5) {

            if(isPost){
              updateParams(client.readString());
            }

            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line:
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println();

            client.print(R"(<!DOCTYPE HTML><html><head><title>Solar Tracker Dashboard</title></head><body>)");


            client.print("<p style=\"font-size:7vw;\">");
            client.print("Wind speed: ");
            client.print(windSensor.getWindSpeed());
            client.print("<br/>");
            client.print("Extended to: ");
            int percentExtended = actuator.getPercentExtended();
            if(percentExtended){
              client.print(percentExtended);
              client.print("%<br/>");
            } else {
              client.print("Please recalibrate");
            }
            client.print("</p>");
            client.print("<br/>");
            client.print("<a href=\"/recalibrate\"><button style=\"font-size:7vw;\">Recalibrate</button></a>");
            client.print(R"(<div style="font-size:7vw;"><form action="/" method="POST">Extend To<input style="font-size:7vw;" type="number" max="100" min ="0" name="extendToPercent"  value=\")");
            client.print(actuator.getPercentExtended());
            client.print(R"("\"><input type="submit" style="font-size:7vw;" value="Submit"></form><div/>)");
            client.print(R"(</body></html>)");

            // The HTTP response ends with another blank line:
            client.println();
            // break out of the while loop:
            break;
          }
          else {      // if you got a newline, then clear currentLine:
            Serial.println(currentLine);
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
          actuator.recalibrate();
          endConnection(client);  
          return;              
        }
      }   
    }
    
    endConnection(client);
  }
}

void WifiModule::updateParams(String params){
  int index = params.indexOf("&");
  bool parseMultiParams = index == -1 ? false : true;
  JsonDocument doc;
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
    doc[title] = description;
  }
  lastParams = doc;

  if(doc["extendToPercent"]){
    String percent = doc["extendToPercent"];
    actuator.extendToPercent(percent.toInt());
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