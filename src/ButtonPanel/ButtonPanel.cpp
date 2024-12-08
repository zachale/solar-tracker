#include "ButtonPanel.h"

void ButtonPanel::setup()
{
    pinMode(WIFI_SERVER_ENABLE_PIN, INPUT_PULLUP);
    pinMode(WIND_SENSOR_ENABLE_PIN, INPUT_PULLUP);
}

bool ButtonPanel::settingsServerEnabled()
{
    return digitalRead(WIFI_SERVER_ENABLE_PIN) == HIGH;
}

bool ButtonPanel::isWindSensorEnabled()
{
    return digitalRead(WIND_SENSOR_ENABLE_PIN) == HIGH;
}

String ButtonPanel::getButtonStatus()
{
    return "Settings Server: " + String(settingsServerEnabled()) + " Wind Sensor: " + String(isWindSensorEnabled());
}