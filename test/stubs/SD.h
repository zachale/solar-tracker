// Host stand-in for the SD library, backed by an in-memory filesystem so
// SettingsManager can be exercised without a card.
#pragma once

#include <Arduino.h>
#include <map>

#define FILE_READ 0
#define FILE_WRITE 1

namespace mock { extern std::map<std::string, std::string> sd_files; extern bool sd_ok; }

// Arduino's SD library is built on the legacy short-name FAT API: names are
// limited to eight characters plus a three character extension. Anything longer
// is rejected by SD.open(), for reading and for writing alike, which the stub
// has to reproduce or it will happily accept names the hardware cannot.
inline bool isValid83(const std::string &n) {
  std::string base = n;
  if (!base.empty() && base[0] == '/') base.erase(0, 1);
  if (base.empty()) return false;
  auto dot = base.find('.');
  if (dot == std::string::npos) return base.size() <= 8;
  if (base.find('.', dot + 1) != std::string::npos) return false;   // only one dot
  return dot <= 8 && dot > 0 && base.size() - dot - 1 <= 3;
}

class File {
public:
  File() : ok(false), pos(0) {}
  File(const std::string &n, int m) : name(n), mode(m), ok(true), pos(0) {
    if (!isValid83(name)) { ok = false; return; }
    if (mode == FILE_WRITE) mock::sd_files[name];      // create if absent
    else if (!mock::sd_files.count(name)) ok = false;
  }
  explicit operator bool() const { return ok; }
  void close() {}
  void println(const String &s) { mock::sd_files[name] += s.s; mock::sd_files[name] += "\n"; }
  void print(const String &s) { mock::sd_files[name] += s.s; }
  int available() {
    return ok ? (int)(mock::sd_files[name].size() - pos) : 0;
  }
  int read() {
    if (!ok || pos >= mock::sd_files[name].size()) return -1;
    return (unsigned char)mock::sd_files[name][pos++];
  }
  String readString() {
    if (!ok) return String();
    String r(mock::sd_files[name].substr(pos));
    pos = mock::sd_files[name].size();
    return r;
  }
  size_t size() { return ok ? mock::sd_files[name].size() : 0; }
  bool seek(size_t p) {
    if (!ok) return false;
    if (p == 0) mock::sd_files[name].clear();   // rewind-to-overwrite, as the firmware uses it
    pos = p;
    return true;
  }
  size_t position() { return pos; }

  // ArduinoJson serialises straight into a File, which means it has to satisfy
  // the Print interface.
  size_t write(uint8_t b) {
    if (!ok) return 0;
    mock::sd_files[name] += (char)b;
    return 1;
  }
  size_t write(const uint8_t *buf, size_t len) {
    if (!ok) return 0;
    mock::sd_files[name].append((const char *)buf, len);
    return len;
  }

private:
  std::string name;
  int mode = FILE_READ;
  bool ok;
  size_t pos;
};

class SDClass {
public:
  bool begin(uint8_t = 0) { return mock::sd_ok; }
  File open(const char *n, int m = FILE_READ) { return File(n, m); }
  bool exists(const char *n) { return isValid83(n) && mock::sd_files.count(n) > 0; }
  bool remove(const char *n) { return isValid83(n) && mock::sd_files.erase(n) > 0; }
};
extern SDClass SD;
