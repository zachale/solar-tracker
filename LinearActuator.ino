#include "./LinearActuator.h"

void LinearActuator::countSteps() {
  if(micros()-lastStepTime > trigDelay){
    steps++;
    lastStepTime = micros();
  }
}

void LinearActuator::updatePosition(){
  pos = (pos + steps) * dir;
  steps = 0;
}

float LinearActuator::getInchesExtended(){
  return conNum*pos;
}

void LinearActuator::home(){
  Serial.println("homing");
  while(isHomed == 0){
    direct(BACKWARD);
    if(prevSteps == steps){
      if(millis() - prevTimer > 50){
        Serial.println("homed!");
        steps = 0;
        isHomed = 1;
      }
    }else{
      prevSteps = steps;
      prevTimer = millis();
    }
  }

  Serial.println("maxing");
  unsigned long timer = millis();
  while(isMaxed == 0){
    direct(FORWARD);
    if(millis() - timer > 1000){
      actuator.updatePosition();
      Serial.println(getInchesExtended());
      timer = millis();
    }
    if(prevSteps == steps){
      if(millis() - prevTimer > 50){
        Serial.println("maxed!");
        isMaxed = 1;
      }
    }else{
      prevSteps = steps;
      prevTimer = millis();
    }
  }
}

void LinearActuator::direct(int inputDir){
  if(inputDir == FORWARD){
    dir = FORWARD;
    setSpeeds(255, 0);
  } else if(inputDir == BACKWARD){
    dir = BACKWARD;
    setSpeeds(0, 255);
  } else {
    setSpeeds(0, 0);
  }
  
}

void LinearActuator::setSpeeds(int forward, int backward){
  analogWrite(10, backward);
  analogWrite(11, forward);
}


