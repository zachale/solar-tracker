#ifndef LA_H
#define LA_H

#include <Arduino.h>

class LinearActuator
{
public:
  int percentExtended = 0;
  bool isHomed = 0;
  bool isMaxed = 0;
  int status = 0;
  static const int FORWARD = 1;
  static const int STOP = 0;
  static const int BACKWARD = -1;
  static const int HOMING = 2;
  static const int MAXING = 3;
  int currentPercentTarget = -1;
  static long steps; // pulses from hall effect sensors
  static void countSteps();
  LinearActuator();
  int getPercentExtended();
  void setExtensionCallBack(void (*)());
  void recalibrate();
  void extendToPercent(float percent);
  void home();
  void max();

private:
  static const int PWMBackwardPin = 5;
  static const int PWMForwardPin = 6;
  static unsigned long lastStepTime;
  static const int trigDelay = 500;
  void (*extensionCallBack)() = nullptr;
  long pos = 0; // Actuator Position in Pulses
  long maxPos = 0;
  int speed = 255;
  unsigned long prevTimer = 0;
  long prevSteps = -1;
  int dir = 0;
  void setStatus(int statusCode);
  void setDirection(int direction);
  void setSpeeds(int forward, int backward);
  void extend(int direction);
  void updatePos();
  int getPosFromPercent(float percent);
  bool hitBoundary();
  void setSpeed(int input);
  bool isExtending();
  void stop();
  bool moveTo(int targetPercent);
  void extendTowardsPos(int targetPos);
};

#endif