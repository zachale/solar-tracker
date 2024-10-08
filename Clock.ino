
#include "./Clock.h"

bool ClockModule::isAlarmTriggered = false;

void ClockModule::setup(){

  pinMode(RTC_POWER_PIN, OUTPUT);
  digitalWrite(RTC_POWER_PIN, HIGH);
  

  for(int i = 0; i < 3; i++){
    if (rtc.begin()) {
      break; 
    }
    digitalWrite(RTC_POWER_PIN, LOW);
    Serial.println("Couldn't find RTC");
    Serial.flush();
    delay(1000);
    digitalWrite(RTC_POWER_PIN, HIGH);
  }

  if (!rtc.begin()) {
    while(1) delay(10);
  }

  rtc.disable32K();

  pinMode(CLOCK_INTERRUPT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CLOCK_INTERRUPT_PIN), this->setAlarmTriggered, FALLING);

  rtc.clearAlarm(1);
  rtc.clearAlarm(2);
  rtc.writeSqwPinMode(DS3231_OFF);
  rtc.disableAlarm(2);

  if (rtc.lostPower()) {
    Serial.println("RTC lost power, let's set the time!");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  if(!rtc.setAlarm1(
            DateTime(2024, 10, 1, 0, 0, 0),
            DS3231_A1_Minute
    )) {
        Serial.println("Error, alarm wasn't set!");
  }else {
      Serial.println("Alarm was set.");
  }

  Serial.println("Setup Clock Module.");
}

bool ClockModule::alarmTriggered(){
  if(!isAlarmTriggered){
    return false;
  }

  isAlarmTriggered = false;
  rtc.clearAlarm(1);
  Serial.println("Alarm detected and cleared.");
  if(!rtc.setAlarm1(
          DateTime(2024, 10, 1, 0, 0, 0),
          DS3231_A1_Minute
  )) {
      Serial.println("Error, alarm wasn't set!");
  }else {
      Serial.println("Alarm was set.");
  }
}

void ClockModule::setAlarmTriggered() {
  isAlarmTriggered = true;
}

uint8_t ClockModule::getHour(){
  return rtc.now().hour();
}

int ClockModule::getPercentOfDay(float hour){
  return  (hour - hourStart)/(hourFinish - hourStart) * 100;
}

int ClockModule::getDayCompletionPercent(){
  uint8_t currentHour = getHour();
  return getPercentOfDay(currentHour);
}

int ClockModule::getHalfDayExtensionPercent(){
  return normalizePercentage(HALF_DAY_PERCENT);
}

int ClockModule::getHourlyExtensionPercent(){
  float dayCompletePercent = getDayCompletionPercent();
  return normalizePercentage(dayCompletePercent);
}

// Normalises a percentage calculated between 'working hours' to an actuator extension percentage
int ClockModule::normalizePercentage(float percent){
  float difference = percentFinish - percentStart;
  return (difference * (percent/100)) + percentStart;
}

bool ClockModule::isActiveHours() {
  return getHour() >= hourStart && getHour() >= hourFinish;
}