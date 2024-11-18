#ifndef BP_H
#define BP_H

#include <Arduino.h>

class ButtonPanel
{
public:
  static bool settingsServerEnabled();
  void setup();  
private:
  static const int WIFI_SERVER_ENABLE_PIN = 7;
};

#endif