#ifndef LA_H
#define LA_H

class LinearActuator {
public:
    long pos = 0;  // Actuator Position in Pulses
    long maxPos = 0;
    long prevPos = 1;                 
    long steps = 0;  //pulses from hall effect sensors
    long prevSteps = 0;              
    float conNum = 0.000285;        // Convert to Inches
    bool isHomed = 0;
    bool isMaxed = 0;              
    unsigned long prevTimer = 0;
    unsigned long lastStepTime = 0; 
    int trigDelay = 500;
    int dir = 0;
    static const int FORWARD = 1;
    static const int BACKWARD = -1;
    void countSteps();
    void updatePosition();
    float getInchesExtended();
    void home();
    void direct(int direction);
    void setSpeeds(int forward, int backward);
};

#endif