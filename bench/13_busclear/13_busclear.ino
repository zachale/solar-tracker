// 13 — I2C bus-clear. The standard recovery for a slave stuck holding SDA low.
//
// NXP UM10204 s3.1.16 "Bus clear": if SDA is stuck low, the master should clock
// SCL up to 9 times so the slave can finish the byte it is stranded in and
// release SDA, then issue a STOP.
//
// This never writes the DS3231 and never sets the clock. It only toggles SCL.

#include <Arduino.h>
#include <Wire.h>

static const uint8_t BUS_SDA = 18;   // A4
static const uint8_t BUS_SCL = 19;   // A5
static const uint8_t ADDR_RTC = 0x68, ADDR_EEP = 0x57;
#define IIC1 R_IIC1

static void regs(const char *tag)
{
  uint8_t c1 = IIC1->ICCR1, c2 = IIC1->ICCR2, s2 = IIC1->ICSR2;
  char b[170];
  snprintf(b, sizeof(b), "REG %s ICCR1=0x%02X[SCLO=%u SDAO=%u SCLI=%u SDAI=%u] ICCR2=0x%02X[BBSY=%u] ICSR2=0x%02X",
           tag, c1,(c1>>3)&1,(c1>>2)&1,(c1>>1)&1,c1&1, c2,(c2>>7)&1, s2);
  Serial.println(b);
}

static uint8_t probe(uint8_t a)
{
  Wire.beginTransmission(a);
  return Wire.endTransmission();
}

static void status(const char *tag)
{
  Serial.print("BUS ");
  Serial.print(tag);
  Serial.print(" ds3231=");
  Serial.print(probe(ADDR_RTC));
  Serial.print(" at24c32=");
  Serial.print(probe(ADDR_EEP));
  Serial.print("  SDA_line=");
  Serial.print(digitalRead(BUS_SDA));
  Serial.print(" SCL_line=");
  Serial.println(digitalRead(BUS_SCL));
  regs(tag);
}

// The recovery itself.
static bool busClear()
{
  Serial.println("BUS --- releasing pins from the IIC peripheral ---");
  Wire.end();
  delay(10);

  pinMode(BUS_SDA, INPUT_PULLUP);
  pinMode(BUS_SCL, INPUT_PULLUP);
  delay(2);
  Serial.print("BUS before: SDA=");
  Serial.print(digitalRead(BUS_SDA));
  Serial.print(" SCL=");
  Serial.println(digitalRead(BUS_SCL));

  if (digitalRead(BUS_SDA) == HIGH)
  {
    Serial.println("BUS SDA already high - nothing stuck on the wire");
  }
  else
  {
    Serial.println("BUS SDA held LOW - clocking SCL to free it");
    pinMode(BUS_SCL, OUTPUT);
    for (int i = 1; i <= 9; i++)
    {
      digitalWrite(BUS_SCL, LOW);  delayMicroseconds(5);
      digitalWrite(BUS_SCL, HIGH); delayMicroseconds(5);
      int sda = digitalRead(BUS_SDA);
      Serial.print("BUS   pulse ");
      Serial.print(i);
      Serial.print(" -> SDA=");
      Serial.println(sda);
      if (sda == HIGH) break;
    }
    // manual STOP: SDA low while SCL high, then SDA released
    pinMode(BUS_SDA, OUTPUT);
    digitalWrite(BUS_SDA, LOW);  delayMicroseconds(5);
    pinMode(BUS_SCL, INPUT_PULLUP); delayMicroseconds(5);
    pinMode(BUS_SDA, INPUT_PULLUP); delayMicroseconds(5);
    Serial.println("BUS STOP issued");
  }

  pinMode(BUS_SDA, INPUT_PULLUP);
  pinMode(BUS_SCL, INPUT_PULLUP);
  bool freed = digitalRead(BUS_SDA) == HIGH;
  Serial.print("BUS after: SDA=");
  Serial.print(digitalRead(BUS_SDA));
  Serial.print(" SCL=");
  Serial.println(digitalRead(BUS_SCL));

  Wire.begin();
  delay(20);
  return freed;
}

void setup()
{
  Serial.begin(9600);
  { unsigned long t0 = millis(); while (!Serial && millis() - t0 < 3000) { ; } }
  delay(1500);
  pinMode(5, OUTPUT); digitalWrite(5, LOW);
  pinMode(6, OUTPUT); digitalWrite(6, LOW);
  pinMode(8, OUTPUT); digitalWrite(8, HIGH);
  delay(200);
  Wire.begin();

  Serial.println();
  Serial.println("BUS ==== 13_busclear ====");
  status("BEFORE");
  Serial.println();
  bool freed = busClear();
  Serial.println();
  status("AFTER ");
  Serial.print("BUS RESULT=");
  Serial.println((probe(ADDR_RTC) == 0) ? "RECOVERED - bus clear worked, no power cycle needed"
                                        : "STILL DEAD - clocking did not free it");
}

void loop()
{
  delay(15000);
  status("poll  ");
}
