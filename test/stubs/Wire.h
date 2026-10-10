// Host stand-in for the Arduino Wire library. The clock's bus health check and
// recovery talk to it directly; on the host the bus is always healthy.
#pragma once

#include <cstdint>

class TwoWire {
public:
  void begin() {}
  void end() {}
  void beginTransmission(uint8_t) {}
  uint8_t endTransmission() { return 0; }
};

inline TwoWire Wire;
