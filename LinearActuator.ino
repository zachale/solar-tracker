#include "./LinearActuator.h"


long LinearActuator::steps = 0;
unsigned long LinearActuator::lastStepTime = 0;


LinearActuator::LinearActuator(){
  pinMode(2, INPUT);
  pinMode(PWMBackwardPin, OUTPUT);
  pinMode(PWMForwardPin, OUTPUT);
}

void LinearActuator::countSteps() {
  if(micros()-lastStepTime > trigDelay){
    steps++;
    lastStepTime = micros();
  }
}

void LinearActuator::updatePos(){
  pos = pos + steps * dir;
  steps = 0;
}

int LinearActuator::getPercentExtended(){
  return percentExtended = (pos / maxPos) * 100;
}

void LinearActuator::recalibrate(){
  Serial.println("Attempting to recalibrate");
  delay(5000);
  home();
  delay(1000);
  max();
}

void LinearActuator::home(){
  Serial.println("homing");
  extend(BACKWARD);
  isHomed == 0;
  while(isHomed == 0){
    setStatus(HOMING);
    if(hitBoundary()){
      pos = 0;
      isHomed = 1;
      break;
    }
  }
  extend(STOP);
}

void LinearActuator::max(){
  Serial.println("maxing");
  extend(FORWARD);
  isMaxed = 0;
  while(isMaxed == 0){
    setStatus(MAXING);
    if(hitBoundary()){
      maxPos = pos;
      isMaxed = 1;
      break;
    }
  }
  extend(STOP);
}

void LinearActuator::extend(int inputDir){
  if(inputDir == FORWARD){
    setDirection(FORWARD);
    setSpeeds(255, 0);
  } else if(inputDir == BACKWARD){
    setDirection(BACKWARD);
    setSpeeds(0, 255);
  } else {
    setSpeeds(0, 0);
  }
  
}

void LinearActuator::setSpeeds(int forward, int backward){
  analogWrite(10, backward);
  analogWrite(11, forward);
}

void LinearActuator::extendToPercent(float percent){
  updatePos();

  if(percent == 0){
    home();
    return;
  } else if(percent == 100) {
    max();
    return;
  }

  float targetPos = getPosFromPercent(percent);
  if(pos < targetPos){
    extend(FORWARD);
  } else {
    extend(BACKWARD);
  }

  Serial.println("Entering Target loop");

  int difference;
  for(difference = 1; difference > 0; difference = (targetPos-pos) * dir){
    Serial.println(difference);
    if(percent != 0 || percent != 100){
      if(hitBoundary()){
        attemptsToRecalibrate++;
        if(attemptsToRecalibrate < 2){
          Serial.println("Recalibrating because of unexpected Boundary");
          recalibrate();
          extendToPercent(percent);
        } else {
          Serial.println("Homing because too many attempts");
          attemptsToRecalibrate = 0;
          home();
        }      
      }
    }
    updatePos();
  }
  Serial.println("Exiting Target loop");

  extend(STOP);
}

int LinearActuator::getPosFromPercent(float percent){
  return maxPos * (percent/100);
}

bool LinearActuator::hitBoundary(){
  if(prevSteps == steps){
    if( millis() - prevTimer > 50){
      Serial.println("boundary hit");
      steps = 0;
      return true;
    }
  } else {
    prevSteps = steps;
    prevTimer = millis();
  }
  return false;
}

void LinearActuator::setStatus(int inputCode){
  status = inputCode;
}

void LinearActuator::setDirection(int direction){
  setStatus(direction);
  dir = direction;
}

