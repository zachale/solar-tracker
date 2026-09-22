// 12 — the A/B/C/D matrix. One sketch, configuration set at compile time.
//
//   CFG_SD   1 = spool a row to the card every SPOOL_MS ; 0 = SD.begin never called
//   CFG_WIFI 1 = associate + TLS GET every SYNC_MS      ; 0 = WiFi never started
//
//   A: CFG_SD=1 CFG_WIFI=0     B: CFG_SD=0 CFG_WIFI=1
//   C: CFG_SD=0 CFG_WIFI=0     D: CFG_SD=1 CFG_WIFI=1   (positive control)
//
// Never writes the DS3231: no adjust, no clearAlarm, no writeSqwPinMode, no rtc.begin.
// Never resets the IIC peripheral. Actuator pins parked low. Purely observational.

#include <Arduino.h>
#include <Wire.h>
#ifndef CFG_SD
#define CFG_SD 0
#endif
#ifndef CFG_WIFI
#define CFG_WIFI 0
#endif
#ifndef CFG_NAME
#define CFG_NAME "X"
#endif

#if CFG_SD || CFG_WIFI
#include <SPI.h>
#include <SD.h>
#endif
#if CFG_WIFI
#include <WiFiS3.h>
#include <ArduinoJson.h>
#include "secrets.h"
#endif

static const uint8_t PIN_MOTOR_B = 5, PIN_MOTOR_F = 6, PIN_RTC_PWR = 8, PIN_SD_CS = 10;
static const uint8_t ADDR_RTC = 0x68, ADDR_EEP = 0x57;
#define IIC1 R_IIC1

#ifndef POLL_MS_V
#define POLL_MS_V 10000
#endif
#ifndef SPOOL_MS_V
#define SPOOL_MS_V 600000
#endif
#ifndef SYNC_MS_V
#define SYNC_MS_V 1800000
#endif
#ifndef I2C_HZ
#define I2C_HZ 100000
#endif
static const unsigned long POLL_MS  = POLL_MS_V;
static const unsigned long SPOOL_MS = SPOOL_MS_V;
static const unsigned long SYNC_MS  = SYNC_MS_V;

static unsigned long polls = 0, wedgeAtMs = 0, spools = 0, syncs = 0, wedgeCount = 0, recovered = 0;
static bool wedged = false;
static bool sdUp = false;

static const uint8_t BUS_SDA = 18, BUS_SCL = 19;   // A4 / A5

// NXP UM10204 3.1.16 bus clear: clock SCL until a stranded slave releases SDA.
// Run at boot so each config starts from a known-good bus, and again whenever a
// wedge is first detected so we learn whether it is recoverable in software.
static bool busClear(const char *why)
{
  Wire.end();
  delay(10);
  pinMode(BUS_SDA, INPUT_PULLUP);
  pinMode(BUS_SCL, INPUT_PULLUP);
  delay(2);
  int before = digitalRead(BUS_SDA);
  int pulses = 0;
  if (before == LOW)
  {
    pinMode(BUS_SCL, OUTPUT);
    for (int i = 1; i <= 9; i++)
    {
      digitalWrite(BUS_SCL, LOW);  delayMicroseconds(5);
      digitalWrite(BUS_SCL, HIGH); delayMicroseconds(5);
      pulses = i;
      if (digitalRead(BUS_SDA) == HIGH) break;
    }
    pinMode(BUS_SDA, OUTPUT);
    digitalWrite(BUS_SDA, LOW); delayMicroseconds(5);
    pinMode(BUS_SCL, INPUT_PULLUP); delayMicroseconds(5);
    pinMode(BUS_SDA, INPUT_PULLUP); delayMicroseconds(5);
  }
  pinMode(BUS_SDA, INPUT_PULLUP);
  pinMode(BUS_SCL, INPUT_PULLUP);
  int after = digitalRead(BUS_SDA);
  Wire.begin();
  Wire.setClock(I2C_HZ);
  delay(20);
  Serial.print("MTX BUSCLEAR why=");
  Serial.print(why);
  Serial.print(" sda_before=");
  Serial.print(before);
  Serial.print(" pulses=");
  Serial.print(pulses);
  Serial.print(" sda_after=");
  Serial.println(after);
  return after == HIGH;
}

static uint8_t probe(uint8_t a, unsigned long *us)
{
  unsigned long t0 = micros();
  Wire.beginTransmission(a);
  uint8_t c = Wire.endTransmission();
  *us = micros() - t0;
  return c;
}

static void regs(const char *tag)
{
  uint8_t c1 = IIC1->ICCR1, c2 = IIC1->ICCR2, s2 = IIC1->ICSR2;
  char b[190];
  snprintf(b, sizeof(b),
    "REG %s ICCR1=0x%02X[ICE=%u RST=%u SCLO=%u SDAO=%u SCLI=%u SDAI=%u] "
    "ICCR2=0x%02X[BBSY=%u MST=%u TRS=%u] ICSR2=0x%02X[TMOF=%u AL=%u NACKF=%u TEND=%u]",
    tag, c1,(c1>>7)&1,(c1>>6)&1,(c1>>3)&1,(c1>>2)&1,(c1>>1)&1,c1&1,
    c2,(c2>>7)&1,(c2>>6)&1,(c2>>5)&1,
    s2,s2&1,(s2>>1)&1,(s2>>4)&1,(s2>>6)&1);
  Serial.println(b);
}

// read the DS3231 time registers directly - no RTClib, no writes
static bool readClock(uint8_t *hh, uint8_t *mm, uint8_t *ss, float *tempC)
{
  Wire.beginTransmission(ADDR_RTC);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(ADDR_RTC, (uint8_t)3) != 3) return false;
  uint8_t s = Wire.read(), m = Wire.read(), h = Wire.read();
  *ss = (s & 0x0F) + ((s >> 4) & 0x07) * 10;
  *mm = (m & 0x0F) + ((m >> 4) & 0x07) * 10;
  *hh = (h & 0x0F) + ((h >> 4) & 0x03) * 10;
  Wire.beginTransmission(ADDR_RTC);
  Wire.write((uint8_t)0x11);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(ADDR_RTC, (uint8_t)2) != 2) return false;
  int8_t t = (int8_t)Wire.read();
  uint8_t f = Wire.read();
  *tempC = t + ((f >> 6) * 0.25f);
  return true;
}

static void report()
{
  polls++;
  unsigned long uR = 0, uE = 0;
  uint8_t cR = probe(ADDR_RTC, &uR), cE = probe(ADDR_EEP, &uE);
  uint8_t hh = 0, mm = 0, ss = 0; float tc = -99;
  bool ok = readClock(&hh, &mm, &ss, &tc);
  bool bad = (cR != 0) || !ok;

  char b[220];
  snprintf(b, sizeof(b),
    "MTX cfg=%s n=%lu ms=%lu rtc=%u(%luus) eep=%u(%luus) clk=%02u:%02u:%02u temp=%.2f ok=%u spools=%lu syncs=%lu%s",
    CFG_NAME, polls, millis(), cR, uR, cE, uE, hh, mm, ss, tc, ok ? 1 : 0, spools, syncs, wedgeCount, recovered,
    bad ? "  <<BAD>>" : "");
  Serial.println(b);
  if (bad) regs("bad ");

  if (bad && !wedged)
  {
    wedged = true;
    wedgeCount++;
    wedgeAtMs = millis();
    Serial.print("MTX WEDGE cfg=");
    Serial.print(CFG_NAME);
    Serial.print(" first_failure_ms=");
    Serial.print(wedgeAtMs);
    Serial.print(" minutes=");
    Serial.println(wedgeAtMs / 60000.0, 2);
    regs("wedge");
    // does software recovery work on this wedge? record it, then keep soaking.
    bool freed = busClear("after_wedge");
    unsigned long u1 = 0;
    uint8_t again = probe(ADDR_RTC, &u1);
    Serial.print("MTX RECOVERY cfg=");
    Serial.print(CFG_NAME);
    Serial.print(" sda_freed=");
    Serial.print(freed ? 1 : 0);
    Serial.print(" rtc_after=");
    Serial.print(again);
    Serial.println(again == 0 ? "  SOFTWARE_RECOVERY_WORKED" : "  STILL_DEAD");
    if (again == 0) { wedged = false; recovered++; }   // keep soaking, count the next one too
  }
}

void setup()
{
  Serial.begin(9600);
  { unsigned long t0 = millis(); while (!Serial && millis() - t0 < 3000) { ; } }
  delay(1500);
  pinMode(PIN_MOTOR_B, OUTPUT); digitalWrite(PIN_MOTOR_B, LOW);
  pinMode(PIN_MOTOR_F, OUTPUT); digitalWrite(PIN_MOTOR_F, LOW);
  pinMode(PIN_RTC_PWR, OUTPUT); digitalWrite(PIN_RTC_PWR, HIGH);
  delay(200);
  Wire.begin();
  Wire.setClock(I2C_HZ);

  Serial.println();
  Serial.print("MTX ==== config ");
  Serial.print(CFG_NAME);
  Serial.print("  SD=");
  Serial.print(CFG_SD);
  Serial.print("  WIFI=");
  Serial.print(CFG_WIFI);
  Serial.println(" ====");

#if CFG_SD
  sdUp = SD.begin(PIN_SD_CS);
  Serial.print("MTX SD.begin ");
  Serial.println(sdUp ? "ok" : "FAILED");
#else
  Serial.println("MTX SD not initialised - zero SPI traffic this run");
#endif
#if !CFG_WIFI
  Serial.println("MTX WiFi never started this run");
#endif
  busClear("boot");
  report();
}

static unsigned long lastPoll = 0, lastSpool = 0, lastSync = 0;

void loop()
{
  if (millis() - lastPoll >= POLL_MS) { lastPoll = millis(); report(); }

#if CFG_SD
  if (sdUp && millis() - lastSpool >= SPOOL_MS)
  {
    lastSpool = millis();
    File f = SD.open("bench12.csv", FILE_WRITE);
    if (f) { if (f.size() < 200000) { f.print(millis()); f.print(",x\n"); } f.close(); spools++; }
    Serial.print("MTX spool n="); Serial.println(spools);
  }
#endif

#if CFG_WIFI
  if (millis() - lastSync >= SYNC_MS || (syncs == 0 && millis() > 60000))
  {
    lastSync = millis();
    syncs++;
    Serial.print("MTX sync n="); Serial.println(syncs);
    File wf = SD.open("wifi.jsn", FILE_READ);
    String ss, pw;
    if (wf) { JsonDocument d; if (!deserializeJson(d, wf)) { for (JsonObject o : d.as<JsonArray>()) { ss = o["ssid"].as<String>(); pw = o["pass"].as<String>(); } } wf.close(); }
    if (ss.length())
    {
      WiFi.end(); delay(300);
      WiFi.begin(ss.c_str(), pw.c_str());
      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
      if (WiFi.status() == WL_CONNECTED)
      {
        WiFiSSLClient c;
        String host = API; if (host.startsWith("https://")) host = host.substring(8);
        if (c.connect(host.c_str(), 443))
        {
          c.print("GET /time/est HTTP/1.1\r\nHost: "); c.print(host); c.print("\r\nConnection: close\r\n\r\n");
          unsigned long r0 = millis(); unsigned long n = 0;
          while (c.connected() && millis() - r0 < 10000) { while (c.available() && n < 16384) { c.read(); n++; } }
          c.stop();
        }
      }
      WiFi.end();
    }
  }
#endif
}
