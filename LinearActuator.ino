#include "./LinearActuator.h"


long LinearActuator::steps = 0;
unsigned long LinearActuator::lastStepTime = 0;

LinearActuator::LinearActuator(void (*inputCallBack)()){
  pinMode(2, INPUT);
  pinMode(PWMBackwardPin, OUTPUT);
  pinMode(PWMForwardPin, OUTPUT);
  extensionCallBack = inputCallBack;
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
  if(!maxPos){
    return -1;
  }
  return percentExtended = ((float)pos / (float)maxPos) * 100;
}

void LinearActuator::recalibrate(){
  Serial.println("Attempting to recalibrate");
  delay(5000);
  home();
  max();
}

void LinearActuator::home(){
  Serial.println("homing");
  extend(BACKWARD);
  isHomed == 0;
  while(isHomed == 0){
    setStatus(HOMING);
    if(extensionCallBack){
      extensionCallBack();
    }
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
    updatePos();
    setStatus(MAXING);
    if(extensionCallBack){
      extensionCallBack();
    }
    if(hitBoundary()){
      maxPos = pos;
      isMaxed = 1;
      break;
    }
  }
  extend(STOP);
}

void LinearActuator::extend(int inputDir){
  prevTimer = millis();
  if(inputDir == FORWARD){
    setDirection(FORWARD);
    setSpeeds(speed, 0);
  } else if(inputDir == BACKWARD){
    setDirection(BACKWARD);
    setSpeeds(0, speed);
  } else {
    speed = 255;
    setSpeeds(0, 0);
    delay(500);
  }
  
}

void LinearActuator::setSpeeds(int forward, int backward){
  analogWrite(PWMBackwardPin, backward);
  analogWrite(PWMForwardPin, forward);
}

void LinearActuator::extendToPercent(float percent){
  Serial.print("Extending to");
  Serial.print(percent);
  Serial.print("\n");
  updatePos();
  if(percent == 0){
    home();
    return;
  } else if(percent == 100) {
    max();
    return;
  }
  if(maxPos == 0){
    recalibrate();
  }
  float targetPos = getPosFromPercent(percent);
  if(pos < targetPos){
    extend(FORWARD);
  } else {
    extend(BACKWARD);
  }
  int difference;
  for(difference = 1; difference > 0; difference = (targetPos-pos) * dir){
    if(hitBoundary()){
      Serial.println("Recalibrating because of unexpected Boundary");
      recalibrate();
      extendToPercent(percent);
      return;
    }
    if(extensionCallBack){
      extensionCallBack();
    }
    if(difference < 255){
      setSpeed(difference + 100);
    } else {
      setSpeed(255);
    }
    updatePos();
  }
  extend(STOP);
}

void LinearActuator::setSpeed(int input){
  speed = input;
  extend(dir);
}

int LinearActuator::getPosFromPercent(float percent){
  return maxPos * (percent/100);
}

bool LinearActuator::hitBoundary(){
  if(prevSteps == steps){
    if( millis() - prevTimer > 1000){
      Serial.println("boundary hit");
      steps = 0;
      prevTimer = millis();
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

