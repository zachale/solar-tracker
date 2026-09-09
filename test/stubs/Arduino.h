// Host-side stand-in for the Arduino core, so the firmware's own .cpp files can be
// compiled and tested natively. Only the surface the firmware actually uses is here.
//
// Time and pin I/O are driven by mock state (see mock.h) rather than hardware, which
// is what makes the timing-dependent logic — wind hysteresis, actuator stall
// detection — testable at all.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <utility>

// ---------------------------------------------------------------- pin constants
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define RISING 3
#define FALLING 2
#define CHANGE 1
#define A0 14
#define A1 15
#define A2 16
#define A3 17

#define F(x) (x)
#define PROGMEM

// ---------------------------------------------------------------- Arduino String
class String {
public:
  std::string s;

  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(const std::string &v) : s(v) {}
  String(char c) : s(1, c) {}
  String(int v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(uint8_t v) : s(std::to_string((int)v)) {}
  String(double v, int decimals = 2) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(decimals) << v;
    s = o.str();
  }

  const char *c_str() const { return s.c_str(); }
  unsigned length() const { return (unsigned)s.size(); }
  bool isEmpty() const { return s.empty(); }
  // Arduino's String uses the safe-bool idiom; ArduinoJson tests writers with it.
  explicit operator bool() const { return true; }
  bool reserve(unsigned n) { s.reserve(n); return true; }

  String &concat(const String &o) { s += o.s; return *this; }
  // ArduinoJson writes into String through this overload.
  bool concat(const char *o, unsigned len) { s.append(o, len); return true; }
  String &concat(const char *o) { s += (o ? o : ""); return *this; }
  String &concat(int v) { s += std::to_string(v); return *this; }

  String &operator+=(const String &o) { s += o.s; return *this; }
  String &operator+=(const char *o) { s += (o ? o : ""); return *this; }

  bool startsWith(const String &p) const { return s.rfind(p.s, 0) == 0; }
  bool endsWith(const String &p) const {
    return s.size() >= p.s.size() && s.compare(s.size() - p.s.size(), p.s.size(), p.s) == 0;
  }
  int indexOf(const String &n) const {
    auto i = s.find(n.s);
    return i == std::string::npos ? -1 : (int)i;
  }
  int indexOf(char c) const {
    auto i = s.find(c);
    return i == std::string::npos ? -1 : (int)i;
  }
  String substring(unsigned from) const {
    return from >= s.size() ? String() : String(s.substr(from));
  }
  String substring(unsigned from, unsigned to) const {
    if (from >= s.size() || to <= from) return String();
    if (to > s.size()) to = (unsigned)s.size();
    return String(s.substr(from, to - from));
  }
  long toInt() const { return std::atol(s.c_str()); }
  float toFloat() const { return (float)std::atof(s.c_str()); }
  void toCharArray(char *buf, unsigned size) const {
    std::snprintf(buf, size, "%s", s.c_str());
  }
  char operator[](unsigned i) const { return i < s.size() ? s[i] : '\0'; }

  bool operator==(const String &o) const { return s == o.s; }
  bool operator!=(const String &o) const { return s != o.s; }
  bool operator<(const String &o) const { return s < o.s; }
};

inline String operator+(const String &a, const String &b) { return String(a.s + b.s); }
inline String operator+(const String &a, const char *b) { return String(a.s + (b ? b : "")); }
inline String operator+(const char *a, const String &b) { return String(std::string(a ? a : "") + b.s); }

// ---------------------------------------------------------------- mocked hardware
namespace mock {
  extern unsigned long now_ms;              // drives millis()
  extern unsigned long now_us;              // drives micros()
  extern int analog[24];                    // analogRead() per pin
  extern int digital[24];                   // digitalRead() per pin
  extern int pwm[24];                       // last analogWrite() per pin
  extern std::vector<std::string> serial;   // captured Serial output
  void reset();
  void advance(unsigned long ms);
}

inline unsigned long millis() { return mock::now_ms; }
inline unsigned long micros() { return mock::now_us; }
inline void delay(unsigned long ms) { mock::advance(ms); }
inline void delayMicroseconds(unsigned long us) { mock::now_us += us; }

inline void pinMode(int, int) {}
inline int digitalRead(int p) { return mock::digital[p]; }
inline void digitalWrite(int p, int v) { mock::digital[p] = v; }
inline int analogRead(int p) { return mock::analog[p]; }
inline void analogWrite(int p, int v) { mock::pwm[p] = v; }
inline int digitalPinToInterrupt(int p) { return p; }
inline void attachInterrupt(int, void (*)(), int) {}
inline void detachInterrupt(int) {}

// ---------------------------------------------------------------- Serial
struct SerialStub {
  void begin(unsigned long) {}
  explicit operator bool() const { return true; }
  template <typename T> void print(const T &v) { mock::serial.push_back(String(v).s); }
  void print(const String &v) { mock::serial.push_back(v.s); }
  void print(const char *v) { mock::serial.push_back(v ? v : ""); }
  template <typename T> void println(const T &v) { print(v); mock::serial.push_back("\n"); }
  void println(const String &v) { print(v); mock::serial.push_back("\n"); }
  void println(const char *v) { print(v); mock::serial.push_back("\n"); }
  void println() { mock::serial.push_back("\n"); }
  void flush() {}
  int available() { return 0; }
  int read() { return -1; }
  void write(uint8_t) {}
};
extern SerialStub Serial;
