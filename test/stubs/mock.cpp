// Definitions for the mocked hardware state declared in the stub headers.
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

SerialStub Serial;
SDClass SD;
SPIStub SPI;

namespace mock {
  unsigned long now_ms = 0;
  unsigned long now_us = 0;
  int analog[24] = {0};
  int digital[24] = {0};
  int pwm[24] = {0};
  std::vector<std::string> serial;
  std::map<std::string, std::string> sd_files;
  bool sd_ok = true;

  void reset() {
    now_ms = 0;
    now_us = 0;
    for (int i = 0; i < 24; i++) { analog[i] = 0; digital[i] = 0; pwm[i] = 0; }
    serial.clear();
    sd_files.clear();
    sd_ok = true;
  }

  void advance(unsigned long ms) {
    now_ms += ms;
    now_us += ms * 1000UL;
  }
}
