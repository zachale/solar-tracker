// Date and time functions using a DS3231 RTC connected via I2C and Wire lib
#include "RTClib.h"

RTC_DS3231 rtc;

const int windSensorPin = A3;
const int PWMBackward = 10;
const int PWMForward = 11;

float voltageConversionConstant = 0.004882814;
int sensorDelay = 1000;

float windVoltageMin = 0.4;
float windSpeedMin = 0;
float windVoltageMax = 2.0;
float windSpeedMax = 32.0;

long pos = 0;  // Actuator Position in Pulses
long prevPos = 1;                 
long steps = 0;  //pulses from hall effect sensors
long prevSteps = 0;              
float conNum = 0.000285;        // Convert to Inches
bool homeFlag = 0;              // Flag use to know if the Actuator is home 
unsigned long prevTimer = 0;
unsigned long lastStepTime = 0; 
int trigDelay = 500;
int dir = 0;


void setup () {
  Serial.begin(57600);

#ifndef ESP8266
  while (!Serial); // wait for serial port to connect. Needed for native USB
#endif

  if (! rtc.begin()) {
    Serial.println("Couldn't find RTC");
    Serial.flush();
    while (1) delay(10);
  }

  if (rtc.lostPower()) {
    Serial.println("RTC lost power, let's set the time!");
    // When time needs to be set on a new device, or after a power loss, the
    // following line sets the RTC to the date & time this sketch was compiled
    // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    // This line sets the RTC with an explicit date & time, for example to set
    // January 21, 2014 at 3am you would call:
    // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
    
  }

  pinMode(PWMBackward, OUTPUT);
  pinMode(PWMForward, OUTPUT);

  pinMode(2, INPUT);
  attachInterrupt(digitalPinToInterrupt(2), countSteps, RISING);


  // When time needs to be re-set on a previously configured device, the
  // following line sets the RTC to the date & time this sketch was compiled
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  // This line sets the RTC with an explicit date & time, for example to set
  // January 21, 2014 at 3am you would call:
  // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
}

void loop () {
    
    float windSpeed = getWindSpeed();

    if(homeFlag != 0){
      homeActuator();
    }
    float temperature = getTemperature();
    uint8_t hour = getHour();
    delay(sensorDelay);
}

int getWindSpeed(){
  float sensorValue = analogRead(windSensorPin);
  float sensorVoltage = sensorValue * voltageConversionConstant;
  if(sensorVoltage <= windVoltageMin){
    return 0;
  } else {

    return (sensorVoltage-windVoltageMin)*windSpeedMax/(windVoltageMax-windVoltageMin);
  }
}

float getTemperature() {
  return rtc.getTemperature();
}

uint8_t getHour(){
  DateTime now = rtc.now();
  return now.hour();
}

void countSteps(void) {
  if(micros()-lastStepTime > trigDelay){
    steps++;
    lastStepTime = micros();
  }
  Serial.println("new step!");
}

void updatePosition(){
  if(dir == 1){
    pos = pos + steps;
    steps = 0;
  } else {
    pos = pos - steps;
    steps = 0;
  }
}

void homeActuator(){
  prevTimer = millis();
  while(homeFlag == 0){
    int Speed = 127;
    analogWrite(10, 0);
    analogWrite(11, Speed);
    if(prevSteps == steps){
      if(millis() - prevTimer > 10){
        analogWrite(10, 0);
        analogWrite(11, 0);
        Serial.println("homed!");
        steps = 0;
        Speed = 0;
        homeFlag = 1;
      }
     }else{
      prevSteps = steps;
      
      prevTimer = millis();
     }
  }
}

float convertToInches(long pos){
  return conNum*pos;
}

bool driveLinearActuator(int windSpeed){
    analogWrite(11, 11);
    analogWrite(10, 0);
}
