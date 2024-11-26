#include "ButtonPanel.h"

void ButtonPanel::setup()
{
    pinMode(WIFI_SERVER_ENABLE_PIN, INPUT_PULLUP);
    pinMode(WIND_SENSOR_ENABLE_PIN, INPUT_PULLUP);
}

bool ButtonPanel::settingsServerEnabled()
{   
    bool test = digitalRead(WIFI_SERVER_ENABLE_PIN) == HIGH;
    if(test){
        Serial.println("Settings server enabled");
        return true;
    }
    return false;
}

bool ButtonPanel::isWindSensorEnabled()
{
    return digitalRead(WIND_SENSOR_ENABLE_PIN) == HIGH;
}