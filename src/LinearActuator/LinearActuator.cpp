#include "./LinearActuator.h"

long LinearActuator::steps = 0;
unsigned long LinearActuator::lastStepTime = 0;

LinearActuator::LinearActuator()
{
  pinMode(2, INPUT);
  pinMode(PWMBackwardPin, OUTPUT);
  pinMode(PWMForwardPin, OUTPUT);
}

void LinearActuator::countSteps()
{
  if (micros() - lastStepTime > trigDelay)
  {
    steps++;
    lastStepTime = micros();
  }
}

void LinearActuator::updatePos()
{
  pos = pos + steps * dir;
  steps = 0;
}

int LinearActuator::getPercentExtended()
{
  if (!maxPos)
  {
    return -1;
  }
  return percentExtended = ((float)pos / (float)maxPos) * 100;
}

void LinearActuator::recalibrate()
{
  Serial.println("Attempting to recalibrate");
  delay(5000);
  home();
  max();
}

void LinearActuator::home()
{
  Serial.println("homing");
  extend(BACKWARD);
  isHomed == 0;
  while (isHomed == 0)
  {
    setStatus(HOMING);
    if (extensionCallBack)
    {
      extensionCallBack();
    }
    if (hitBoundary())
    {
      pos = 0;
      isHomed = 1;
      break;
    }
  }
  extend(STOP);
}

void LinearActuator::max()
{
  Serial.println("maxing");
  extend(FORWARD);
  isMaxed = 0;
  while (isMaxed == 0)
  {
    updatePos();
    setStatus(MAXING);
    if (extensionCallBack)
    {
      extensionCallBack();
    }
    if (hitBoundary())
    {
      maxPos = pos;
      isMaxed = 1;
      break;
    }
  }
  extend(STOP);
}

void LinearActuator::extend(int inputDir)
{
  prevTimer = millis();
  if (inputDir == FORWARD)
  {
    setDirection(FORWARD);
    setSpeeds(speed, 0);
  }
  else if (inputDir == BACKWARD)
  {
    setDirection(BACKWARD);
    setSpeeds(0, speed);
  }
  else
  {
    stop();
    speed = 255;
  }
}

void LinearActuator::stop()
{
  setSpeeds(0, 0);
  setStatus(STOP);
  delay(500);
}

void LinearActuator::setSpeeds(int forward, int backward)
{
  analogWrite(PWMBackwardPin, backward);
  analogWrite(PWMForwardPin, forward);
}

bool LinearActuator::isExtending()
{
  return status == FORWARD || status == BACKWARD || status == HOMING || status == MAXING;
}

void LinearActuator::extendToPercent(float percent)
{
  if (percent == currentPercentTarget)
  {
    Serial.println("Same extension target detected, ignoring");
    return;
  }

  currentPercentTarget = percent;

  // If the actuator is already extending, then we should update the target percent to redirect the current extension
  if (status != STOP)
  {
    Serial.println("New extension target detected, aborting current request, and updated target");
    return;
  }

  updatePos();

  if (percent == 0)
  {
    home();
    return;
  }
  else if (percent == 100)
  {
    max();
    return;
  }

  if (maxPos == 0)
  {
    recalibrate();
  }

  for (int i = 0; i < 2; i++)
  {
    if (moveTo(percent))
    {
      return;
    }
    Serial.println("Failed to move to target, recalibrating and retrying");
    recalibrate();
  };
  Serial.println("Failed to move to target.");
  return;
}

// Move the actuator to a target percentage, and return true if successful
// Automatically retries if the actuator hits an unexpected boundary or if the target percentage changes
bool LinearActuator::moveTo(int targetPercent)
{
  // Ensure all extensions have stopped
  extend(STOP);

  Serial.println("Extending to " + String(targetPercent));

  float targetPos = getPosFromPercent(targetPercent);

  // Determine direction of target from current position
  extendTowardsPos(targetPos);

  for (int difference = 1; difference > 0; difference = (targetPos - pos) * dir)
  {

    // When the actuator is approaching the target, slow down the speed
    if (difference < 255)
    {
      setSpeed(difference + 100);
    }
    else
    {
      setSpeed(255);
    }

    // If the actuator hits an unexpected boundary, then we should recalibrate and exit
    if (hitBoundary())
    {
      Serial.println("Unexpected boundary hit");
      return false;
    }

    if (extensionCallBack)
    {
      extensionCallBack();
      if (targetPercent != currentPercentTarget)
      {
        targetPos = getPosFromPercent(currentPercentTarget);
        extendTowardsPos(targetPos);
      }
    }
    updatePos();
  }
  extend(STOP);
  return true;
}

void LinearActuator::extendTowardsPos(int targetPos)
{
  Serial.println("Extending towards " + String(targetPos));
  Serial.println("Starting Position " + String(pos));
  // Determine direction of target from current position
  if (pos < targetPos)
  {
    extend(FORWARD);
  }
  else
  {
    extend(BACKWARD);
  }
}

void LinearActuator::setSpeed(int input)
{
  speed = input;
  extend(dir);
}

int LinearActuator::getPosFromPercent(float percent)
{
  return maxPos * (percent / 100);
}

bool LinearActuator::hitBoundary()
{
  if (prevSteps == steps)
  {
    if (millis() - prevTimer > 1000)
    {
      Serial.println("boundary hit");
      steps = 0;
      prevTimer = millis();
      return true;
    }
  }
  else
  {
    prevSteps = steps;
    prevTimer = millis();
  }
  return false;
}

void LinearActuator::setStatus(int inputCode)
{
  status = inputCode;
}

void LinearActuator::setDirection(int direction)
{
  setStatus(direction);
  dir = direction;
}

void LinearActuator::setExtensionCallBack(void (*inputCallBack)())
{
  extensionCallBack = inputCallBack;
}