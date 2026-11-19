#include "./LinearActuator.h"

long LinearActuator::steps = 0;
unsigned int LinearActuator::recalibrations = 0;
unsigned int LinearActuator::moveTimeouts = 0;
unsigned long LinearActuator::lastStepTime = 0;
unsigned long LinearActuator::lastRecalibrationTime = 0;

LinearActuator::LinearActuator()
{
  pinMode(2, INPUT);
  pinMode(PWMBackwardPin, OUTPUT);
  pinMode(PWMForwardPin, OUTPUT);
  lastRecalibrationTime = millis(); // Initialize last recalibration time
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

bool LinearActuator::calibrationDue()
{
  // Latched once a week is up: millis() wraps after 49.7 days, and a calibration that came
  // due during a long away must not stop being due when it does.
  if (millis() - lastRecalibrationTime >= RECALIBRATION_INTERVAL)
  {
    calibrationRequested = true;
  }
  return maxPos == 0 || calibrationRequested;
}

int LinearActuator::recalibrate()
{
  if (isExtending())
  {
    // Asked for from inside a move or a sweep (the dashboard's button, served by the loop the
    // motion calls back into). A sweep nested in a move or in another sweep wrecks both, so it
    // is owed instead, and runs at the next move or sync that may calibrate.
    Serial.println("Recalibration asked for while moving: owed");
    calibrationRequested = true;
    return -1;
  }
  Serial.println("Attempting to recalibrate");
  // Cleared for the sweep, so anything asked for during it is recorded and returned, the
  // target held before it included: extendToPercent() ignores a request equal to the target.
  currentPercentTarget = -1;
  delay(5000);
  home();
  max();
  recalibrations++;
  lastRecalibrationTime = millis();
  calibrationRequested = false;
  int asked = currentPercentTarget;
  // The sweep ends at full extension: whatever was last asked for has to be asked again.
  currentPercentTarget = -1;
  return asked;
}

void LinearActuator::recalibrateInPlace(int fallbackPercent)
{
  int target = currentPercentTarget;
  int asked = recalibrate();
  extendToPercent(asked >= 0 ? asked : target >= 0 ? target : fallbackPercent);
}

LinearActuator::Snapshot LinearActuator::snapshot()
{
  updatePos(); // pulses counted since the last pass, the coast after a stop
  return Snapshot{pos, maxPos, millis() - lastRecalibrationTime, currentPercentTarget, calibrationDue()};
}

bool LinearActuator::restore(const Snapshot &s)
{
  if (s.maxPos <= 0 || s.pos < 0 || s.pos > s.maxPos)
  {
    return false;
  }
  pos = s.pos;
  maxPos = s.maxPos;
  currentPercentTarget = s.target;
  lastRecalibrationTime = millis() - s.sinceCalibrationMs; // unsigned: wraps correctly near boot
  calibrationRequested = s.owed;
  return true;
}

void LinearActuator::home()
{
  Serial.println("homing");
  extend(BACKWARD);
  isHomed = 0;
  unsigned long legStart = millis();
  long startSteps = steps;
  long legPulses = 0;
  unsigned long passes = 0;
  bool boundary = false;
  while (isHomed == 0)
  {
    setStatus(HOMING);
    if (extensionCallBack)
    {
      extensionCallBack();
    }
    passes++;
    legPulses = steps - startSteps; // home() never folds steps into pos, so they add up here
    boundary = hitBoundary();
    // The time limit keeps a sweep that never sees its end (a hall left floating, picking up
    // noise) from holding the actuator off STOP for good, and with it every sync and OTA check.
    if (boundary || millis() - legStart > SWEEP_TIMEOUT_MS)
    {
      pos = 0;
      isHomed = 1;
      break;
    }
  }
  recordLeg(boundary ? 'H' : 'h', legStart, legPulses, passes);
  extend(STOP);
}

void LinearActuator::max()
{
  Serial.println("maxing");
  extend(FORWARD);
  isMaxed = 0;
  unsigned long legStart = millis();
  long startPos = pos;
  unsigned long passes = 0;
  bool boundary = false;
  while (isMaxed == 0)
  {
    updatePos();
    setStatus(MAXING);
    if (extensionCallBack)
    {
      extensionCallBack();
    }
    passes++;
    boundary = hitBoundary();
    if (boundary || millis() - legStart > SWEEP_TIMEOUT_MS) // see home()
    {
      maxPos = pos;
      isMaxed = 1;
      break;
    }
  }
  recordLeg(boundary ? 'M' : 'm', legStart, pos - startPos, passes);
  extend(STOP);
}

void LinearActuator::recordLeg(char kind, unsigned long startMs, long pulses, unsigned long passes)
{
  // First, before anything else can take time: how long since the last hall pulse as the leg
  // ended. Loaded before micros() is read, so the subtraction cannot go negative. At a real end
  // stop it is over a second; a leg that ended with pulses still arriving reads a few ms.
  unsigned long last = lastStepTime;
  unsigned long gapMs = (micros() - last) / 1000UL;
  Leg &leg = legLog[legsRecorded % LEG_LOG];
  leg.kind = kind;
  leg.ms = millis() - startMs;
  leg.pulses = pulses;
  leg.passes = passes;
  leg.gapMs = gapMs;
  legsRecorded++;
}

String LinearActuator::legsQuery()
{
  String q = "&mp=" + String(maxPos) + "&p=" + String(pos) + "&mto=" + String(moveTimeouts) +
             "&legn=" + String(legsRecorded);
  if (legsRecorded == 0)
  {
    return q;
  }
  q += "&legs=";
  unsigned int first = legsRecorded > (unsigned int)LEG_LOG ? legsRecorded - LEG_LOG : 0;
  for (unsigned int i = first; i < legsRecorded; i++)
  {
    const Leg &leg = legLog[i % LEG_LOG];
    if (i != first)
    {
      q += "_";
    }
    q += String(leg.kind) + String(leg.ms) + "." + String(leg.pulses) + "." + String(leg.passes) + "." +
         String(leg.gapMs);
  }
  return q;
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
  // Past either end is a position that does not exist; a move there could never finish.
  if (percent < 0)
  {
    percent = 0;
  }
  else if (percent > 100)
  {
    percent = 100;
  }

  bool mayCalibrate = calibrationAllowed || (maxPos == 0 && positionCalibrationAllowed);
  if (!mayCalibrate && maxPos == 0)
  {
    if (status != STOP)
    {
      // Asked for during the sweep that is finding the position (SAFE, say): recorded, so the
      // sweep's caller goes there once the position is known.
      currentPercentTarget = percent;
    }
    // Otherwise before the target is recorded, so the same request goes through once it may
    // calibrate.
    Serial.println("Position unknown and no calibration allowed now: holding still");
    return;
  }

  if (mayCalibrate && calibrationDue() && status == STOP)
  {
    Serial.println(maxPos == 0 ? "Position unknown: calibrating before the move"
                               : "Calibration due: calibrating before the move");
    int asked = recalibrate();
    if (asked >= 0)
    {
      percent = asked; // a newer request arrived during the sweep (into away or SAFE)
    }
  }

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

  if (!moveTo(percent))
  {
    // Out of time. The pulses counted so far are still right, so the position stands and no
    // calibration is run for it; the weekly one corrects any drift. The target is forgotten,
    // so the same request is tried again rather than taken as done.
    Serial.println("Holding here");
    currentPercentTarget = -1;
    extend(STOP);
  }
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

  // A target the actuator cannot reach (its end stop, an obstruction, a stall on the slow
  // approach) would otherwise be driven for ever: the stall check cannot fire inside a move.
  // The limit restarts whenever the target changes, so a redirect (SAFE, away, the next hour)
  // always gets a full window of its own.
  int timedTarget = currentPercentTarget;
  unsigned long timedSince = millis();

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
        // Once per new target (kept from v12.11 as bookkeeping; the stall check plays no part
        // here). Without it a redirect back to the move's first target was never seen: the move
        // ended at the target in between, and later requests for the first were ignored.
        targetPercent = currentPercentTarget;
        targetPos = getPosFromPercent(targetPercent);
        extendTowardsPos(targetPos);
      }
    }
    updatePos();

    if (currentPercentTarget != timedTarget)
    {
      timedTarget = currentPercentTarget;
      timedSince = millis();
    }
    if (millis() - timedSince > MOVE_TIMEOUT_MS)
    {
      Serial.println("Move did not finish within " + String(MOVE_TIMEOUT_MS / 1000UL) + " s");
      moveTimeouts++;
      return false;
    }
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

// Through extend(), as in v12.10: this restarts the stall timer on every pass of a move.
void LinearActuator::setSpeed(int input)
{
  speed = input;
  extend(dir);
}

int LinearActuator::getPosFromPercent(float percent)
{
  return maxPos * (percent / 100);
}

// Stalled: the pulse count has not changed for a second (prevTimer, also set by extend()).
// This is v12.10's check, restored. It compares the count the hall interrupt writes, never a
// timestamp against the clock: v12.11-v12.13 read micros() and then the interrupt's
// lastStepTime, so a pulse landing between the two read as a second without one, and sweeps
// ended wherever that happened. Inside a move setSpeed() restarts the timer on every pass, so
// this never fires there; moveTo() is bounded by MOVE_TIMEOUT_MS instead.
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
