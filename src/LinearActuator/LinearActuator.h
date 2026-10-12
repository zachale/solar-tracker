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
  static unsigned int recalibrations; // full home-and-max sweeps since boot
  static void countSteps();
  LinearActuator();
  int getPercentExtended();
  void setExtensionCallBack(void (*)());

  // A calibration is a full sweep: all the way home, then all the way out. It is due when
  // the position is unknown, when a move went wrong, or a week after the last one, and it
  // stays due until something that may calibrate runs it.
  bool calibrationDue();
  // Whether a calibration may run now. The tracker keeps this current with its status:
  // never in away (the panel is meant to stay put) or SAFE (the wind it is hiding from).
  // Without it a move never sweeps: with the position unknown it holds still, and if it
  // runs into something it stops where it is and owes a calibration.
  bool calibrationAllowed = true;
  // Returns a target asked for during the sweep (the sweep calls back into the main loop,
  // which can change the status), or -1 if none was.
  int recalibrate();
  // Calibrate, then go to whatever was asked for during the sweep, else back to the
  // position last asked for, else `fallbackPercent`: not left at full extension.
  void recalibrateInPlace(int fallbackPercent);
  void extendToPercent(float percent);
  void home();
  void max();

  // What the next firmware needs to start where this one stopped, across an OTA restart.
  struct Snapshot
  {
    long pos;
    long maxPos;
    unsigned long sinceCalibrationMs;
    int target;
    bool owed; // a calibration was due and waiting
  };
  Snapshot snapshot();
  bool restore(const Snapshot &s);

private:
  static const int PWMBackwardPin = 5;
  static const int PWMForwardPin = 6;
  static unsigned long lastStepTime;
  static const int trigDelay = 500;
  static unsigned long lastRecalibrationTime;                    // Track when we last recalibrated
  static const unsigned long RECALIBRATION_INTERVAL = 604800000; // 7 days in milliseconds
  bool calibrationRequested = false; // a move went wrong where it was not allowed to calibrate
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