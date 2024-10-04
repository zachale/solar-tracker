#ifndef WS_H
#define WS_H

class WindSpeedSensor {
  public:
    int status;
    const static int ACTIVE = 1;
    const static int GUST_DETECTED = 2;
    const static int HIGH_WIND = 3;
    int getSpeed();
    void setUpperSpeedMax(float);
    float getUpperSpeedMax();
    void setLowerSpeedMax(float);
    float getLowerSpeedMax();
    void setHighSpeedDelay(unsigned long);
    unsigned long getHighSpeedDelay();
    void setLowSpeedDelay(unsigned long);
    unsigned long getLowSpeedDelay();
    bool highWindCheck();
  private:
    const static int WIND_SENSOR_PIN = A3;
    unsigned long highSpeedDelay = 60000;
    unsigned long lowSpeedDelay = 900000;
    unsigned long highSpeedTimer = 0;
    unsigned long lowSpeedTimer = 0;
    float upperSpeedMax = 24.0;
    float lowerSpeedMax = 16.0;
    float windVoltageMin = 0.4;
    float windSpeedMax = 32.0;
    float windSpeedMin = 0;
    float windVoltageMax = 2.0;
    void setStatus(int);
    bool isMaintainingHighSpeed();
    bool isMaintainingLowSpeed();
};
#endif