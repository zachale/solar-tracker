// 16_bridge_reset — restart the ESP32-S3 bridge normally, from the RA4M1.
//
// Sends the bridge's own +RESET AT command, which calls ESP.restart(): an ordinary
// reboot, not the download mode that +RESTARTBOOTLOADER or the HID 0xAA report select.
// For clearing a wedged bridge remotely when nobody can pull the USB cable.
#include <WiFiS3.h>
#include "Modem.h"
#include "WiFiCommands.h"
using namespace std;

void setup()
{
  Serial.begin(9600);
  delay(5000);
  Serial.println("RESETTER: asking the bridge to restart");
  string res = "";
  bool ok = modem.write(string(PROMPT(_RESET)), res, "%s", CMD(_RESET));
  Serial.println(String("RESETTER: +RESET answered ") + (ok ? "OK" : "nothing (bridge busy or restarting)"));
}

void loop()
{
  static unsigned long last = 0;
  if (millis() - last >= 10000)
  {
    last = millis();
    Serial.println("RESETTER: alive, bridge firmware " + String(WiFi.firmwareVersion()));
  }
}
