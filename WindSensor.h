#ifndef WS_H
#define WS_H

class WindSensor {
  public:
    int getWindSpeed();
    void setWindSpeedMax(float);
    float getWindSpeedMax();
  private:
    static const int windSensorPin = A3;
    float windSpeedMax = 32.0;
    float windVoltageMin = 0.4;
    float windSpeedMin = 0;
    float windVoltageMax = 2.0;
};

#endif