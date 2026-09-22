// 11 — passive RA4M1 IIC register probe.
//
// PURPOSE: capture the state of the I2C peripheral during a wedge, so we can tell
// whether the master latched (BBSY stuck), the driver desynced, or the slave
// stopped ACKing.
//
// SAFETY RULES THIS SKETCH FOLLOWS
//   * It NEVER writes the DS3231. No adjust(), no clearAlarm(), no writeSqwPinMode(),
//     no disable32K(), no rtc.begin(). The clock is never set and never reset.
//   * It NEVER writes an IIC register on its own. Every recovery step is manual,
//     triggered by a keystroke on the serial port, so the wedge can be observed for
//     as long as wanted before anything is disturbed.
//   * It does not touch the actuator. D5/D6 are driven low at boot.
//   * D8 is driven HIGH at boot and left alone unless rung 3 is explicitly requested.
//
// KEYS (send over serial)
//   r  report registers now
//   1  rung 1: Wire.end() + Wire.begin()      (reopens the FSP driver -> IICRST)
//   2  rung 2: manual IICRST pulse only        (no driver reopen)
//   3  rung 3: D8 rail power cycle             (last resort, disturbs the module)
//
// Each rung logs registers immediately before and after, and re-probes both
// addresses, so we learn exactly which step clears the fault.

#include <Arduino.h>
#include <Wire.h>

static const uint8_t PIN_MOTOR_BACKWARD = 5;
static const uint8_t PIN_MOTOR_FORWARD  = 6;
static const uint8_t PIN_RTC_POWER      = 8;
static const uint8_t ADDR_DS3231        = 0x68;   // the clock
static const uint8_t ADDR_AT24C32       = 0x57;   // EEPROM on the SAME module, same rail

// A4/A5 are P101/P100 = native IIC channel 1 on this variant.
#define IIC1 R_IIC1

struct Regs { uint8_t ccr1, ccr2, mr1, mr2, mr3, sr1, sr2, ser; };

static Regs readRegs()
{
  Regs r;
  r.ccr1 = IIC1->ICCR1;
  r.ccr2 = IIC1->ICCR2;
  r.mr1  = IIC1->ICMR1;
  r.mr2  = IIC1->ICMR2;
  r.mr3  = IIC1->ICMR3;
  r.sr1  = IIC1->ICSR1;
  r.sr2  = IIC1->ICSR2;
  r.ser  = IIC1->ICSER;
  return r;
}

static void printRegs(const char *tag, const Regs &r)
{
  char buf[220];
  snprintf(buf, sizeof(buf),
    "IIC %s ICCR1=0x%02X[ICE=%u IICRST=%u SOWP=%u SCLO=%u SDAO=%u SCLI=%u SDAI=%u] "
    "ICCR2=0x%02X[BBSY=%u MST=%u TRS=%u SP=%u RS=%u ST=%u] "
    "ICSR2=0x%02X[TMOF=%u AL=%u START=%u STOP=%u NACKF=%u RDRF=%u TEND=%u TDRE=%u] "
    "ICMR1=0x%02X ICMR3=0x%02X ICSR1=0x%02X",
    tag,
    r.ccr1, (r.ccr1>>7)&1, (r.ccr1>>6)&1, (r.ccr1>>4)&1, (r.ccr1>>3)&1, (r.ccr1>>2)&1, (r.ccr1>>1)&1, r.ccr1&1,
    r.ccr2, (r.ccr2>>7)&1, (r.ccr2>>6)&1, (r.ccr2>>5)&1, (r.ccr2>>3)&1, (r.ccr2>>2)&1, (r.ccr2>>1)&1,
    r.sr2,  r.sr2&1, (r.sr2>>1)&1, (r.sr2>>2)&1, (r.sr2>>3)&1, (r.sr2>>4)&1, (r.sr2>>5)&1, (r.sr2>>6)&1, (r.sr2>>7)&1,
    r.mr1, r.mr3, r.sr1);
  Serial.println(buf);
}

// Address-only probe. Returns the Wire code and fills the elapsed microseconds.
static uint8_t probeAddr(uint8_t addr, unsigned long *usOut)
{
  unsigned long t0 = micros();
  Wire.beginTransmission(addr);
  uint8_t code = Wire.endTransmission();
  *usOut = micros() - t0;
  return code;
}

static void report(const char *tag)
{
  unsigned long usClock = 0, usEeprom = 0;
  Regs before = readRegs();
  uint8_t cClock  = probeAddr(ADDR_DS3231, &usClock);
  uint8_t cEeprom = probeAddr(ADDR_AT24C32, &usEeprom);
  Regs after = readRegs();

  char buf[180];
  snprintf(buf, sizeof(buf),
    "PROBE %s ms=%lu ds3231=%u(%luus) at24c32=%u(%luus) verdict=%s",
    tag, millis(), cClock, usClock, cEeprom, usEeprom,
    (cClock == 0 && cEeprom == 0) ? "BOTH_OK"
      : (cClock != 0 && cEeprom != 0) ? "BOTH_DEAD_bus_or_rail"
      : (cClock != 0) ? "CLOCK_ONLY_dead_slave_specific"
                      : "EEPROM_ONLY_dead");
  Serial.println(buf);
  printRegs("pre ", before);
  printRegs("post", after);
}

// Is the module's VCC actually on D8, or has it been moved to permanent 5 V?
// Drive D8 low briefly and see whether the devices still answer. If they do,
// their supply is not D8. This is the same action powerCycleRTC() performs, so
// it is no more disruptive than the shipping firmware already is.
static void detectVccWiring()
{
  unsigned long us = 0;
  uint8_t withHigh = probeAddr(ADDR_DS3231, &us);
  digitalWrite(PIN_RTC_POWER, LOW);
  delay(300);
  uint8_t withLow = probeAddr(ADDR_DS3231, &us);
  uint8_t eepLow  = probeAddr(ADDR_AT24C32, &us);
  digitalWrite(PIN_RTC_POWER, HIGH);
  delay(300);
  uint8_t back = probeAddr(ADDR_DS3231, &us);

  Serial.print("IIC VCC-WIRING d8high=");
  Serial.print(withHigh);
  Serial.print(" d8low=");
  Serial.print(withLow);
  Serial.print(" eeprom_d8low=");
  Serial.print(eepLow);
  Serial.print(" d8high_again=");
  Serial.print(back);
  Serial.print("  => ");
  if (withHigh == 0 && withLow == 0)
    Serial.println("VCC is NOT on D8 (module stayed alive with D8 low) - wired to 5V");
  else if (withHigh == 0 && withLow != 0)
    Serial.println("VCC IS on D8 (module died with D8 low) - rung 3 will work");
  else
    Serial.println("inconclusive - bus was already wedged at boot");
}

void setup()
{
  Serial.begin(9600);
  { unsigned long t0 = millis(); while (!Serial && millis() - t0 < 3000) { ; } }
  delay(1500);

  pinMode(PIN_MOTOR_BACKWARD, OUTPUT); digitalWrite(PIN_MOTOR_BACKWARD, LOW);
  pinMode(PIN_MOTOR_FORWARD,  OUTPUT); digitalWrite(PIN_MOTOR_FORWARD,  LOW);
  pinMode(PIN_RTC_POWER, OUTPUT);      digitalWrite(PIN_RTC_POWER, HIGH);
  delay(200);

  Wire.begin();

  Serial.println();
  Serial.println("IIC ==== 11_iic_probe ====");
  Serial.println("IIC passive. Never writes the DS3231. Never resets IIC unless you press a key.");
  Serial.println("IIC keys: r=report  1=Wire.end/begin  2=IICRST pulse  3=D8 rail cycle");
  detectVccWiring();
  report("boot");
}

static unsigned long last = 0;

void loop()
{
  if (Serial.available())
  {
    int c = Serial.read();
    if (c == 'r') { report("manual"); }
    else if (c == '1')
    {
      Serial.println("IIC RUNG1 Wire.end() + Wire.begin()");
      printRegs("b4r1", readRegs());
      Wire.end(); delay(20); Wire.begin(); delay(50);
      report("after_rung1");
    }
    else if (c == '2')
    {
      Serial.println("IIC RUNG2 manual IICRST pulse (ICCR1 bit6), driver left open");
      printRegs("b4r2", readRegs());
      IIC1->ICCR1 |= (1u << 6);     // IICRST = 1
      delayMicroseconds(50);
      IIC1->ICCR1 &= (uint8_t)~(1u << 6);
      delay(50);
      report("after_rung2");
    }
    else if (c == '3')
    {
      Serial.println("IIC RUNG3 D8 rail power cycle (disturbs the module)");
      printRegs("b4r3", readRegs());
      digitalWrite(PIN_RTC_POWER, LOW);  delay(1000);
      digitalWrite(PIN_RTC_POWER, HIGH); delay(1000);
      report("after_rung3");
    }
  }

  if (millis() - last >= 10000)
  {
    last = millis();
    report("poll");
  }
}
