#include "ButtonPanel.h"

void ButtonPanel::setup()
{
    pinMode(WIFI_SERVER_ENABLE_PIN, INPUT);
}

bool ButtonPanel::settingsServerEnabled()
{
    return digitalRead(WIFI_SERVER_ENABLE_PIN) == HIGH;
}