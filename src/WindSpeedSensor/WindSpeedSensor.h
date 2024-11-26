#ifndef WS_H
#define WS_H

#include <Arduino.h>
#include "../ButtonPanel/ButtonPanel.h"

class WindSpeedSensor
{
public:
  enum Status {
    DISABLED,
    ACTIVE,
    GUST_DETECTED,
    HIGH_WIND
  };
  Status status;
  int getSpeed();
  void setUpperSpeedMax(float);
  float getUpperSpeedMax();
  void setLowerSpeedMax(float);
  float getLowerSpeedMax();
  void setUpperSpeedDelay(unsigned long);
  unsigned long getUpperSpeedDelay();
  void setLowerSpeedDelay(unsigned long);
  unsigned long getLowerSpeedDelay();
  bool enteringHighWind();
  bool exitingHighWind();
  bool isEnabled();

private:
  const static int WIND_SENSOR_PIN = A3;
  const static int WIND_SENSOR_ENABLE_PIN = 4;
  const float voltageConversionConstant = 0.004882814;
  unsigned long upperSpeedDelay = 6000;
  unsigned long lowerSpeedDelay = 90000;
  unsigned long upperSpeedTimer = 0;
  unsigned long lowerSpeedTimer = 0;
  float upperSpeedMax = 24.0;
  float lowerSpeedMax = 16.0;
  float windVoltageMin = 0.4;
  float windSpeedMax = 32.0;
  float windSpeedMin = 0;
  float windVoltageMax = 2.0;
  void setStatus(Status);
  bool isMaintainingUpperSpeed();
  bool isMaintainingLowerSpeed();
};
#endif