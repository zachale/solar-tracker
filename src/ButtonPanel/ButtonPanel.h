#ifndef BP_H
#define BP_H

#include <Arduino.h>

class ButtonPanel
{
public:
  static bool settingsServerEnabled();
  static bool isWindSensorEnabled();
  static bool isAwayModeOverride();
  static String getButtonStatus();
  void setup();

private:
  static const int WIFI_SERVER_ENABLE_PIN = 7;
  static const int WIND_SENSOR_ENABLE_PIN = 4;
  static const int AWAY_MODE_OVERRIDE_PIN = A2;
};

#endif