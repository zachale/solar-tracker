#ifndef LA_H
#define LA_H

class LinearActuator {
  public:
    int percentExtended = 0;
    bool isHomed = 0;
    bool isMaxed = 0;              
    static const int FORWARD = 1;
    static const int STOP = 0;
    static const int BACKWARD = -1;
    static const int HOMING = 2;
    static const int MAXING = 3;
    static long steps;  //pulses from hall effect sensors
    static void countSteps();
    LinearActuator();
    int getPercentExtended();
    void recalibrate();
    void extendToPercent(float percent);
  private:
    static const int PWMBackwardPin = 10;
    static const int PWMForwardPin = 11;
    static unsigned long lastStepTime; 
    static const int trigDelay = 500;
    long pos = 0;  // Actuator Position in Pulses
    long maxPos = 0;
    unsigned long prevTimer = 0;
    long prevSteps = -1;    
    int status = 0;              
    int dir = 0;
    int attemptsToRecalibrate = 0;
    void setStatus(int statusCode);
    void setDirection(int direction);
    void setSpeeds(int forward, int backward);
    void home();
    void max();
    void extend(int direction);
    void updatePos();
    int getPosFromPercent(float percent);
    bool hitBoundary();
};

#endif