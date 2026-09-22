# Wiring

Single-axis solar tracker. Arduino UNO R4 WiFi, 5 V logic, one 12 V supply.
Pins read from the working tree on `feature/telemetry`.

## Header map

Used pins only. Everything else on both headers is free.

```
DIGITAL   SCL SDA AREF GND D13 D12 D11 D10 D9 | D8 D7 D6 D5 D4 D3 D2 D1 D0
           .   .    -   *   *   *   *   *  -  |  *  *  *  *  *  *  *  -  -

POWER/A   NC IOREF RST 3V3 5V GND GND VIN | A0 A1 A2 A3 A4 A5
           -   -    -   -  *   *   *   *  |  -  -  *  *  *  *
```

`*` used · `.` duplicate of A4/A5, do not wire both · `-` free

## Connections

| Pin | Mode | Goes to | Notes |
| --- | --- | --- | --- |
| D2  | `INPUT`, IRQ RISING | Actuator hall — signal | No pull-up. Confirm the hall's output type and level first. |
| D3  | `INPUT_PULLUP`, IRQ FALLING | DS3231 `INT/SQW` | Open-drain. ZS-042 has its own pull-up. |
| D4  | `INPUT_PULLUP` | Panel switch — wind sensor | Open = enabled. |
| D5  | `OUTPUT`, PWM 490 Hz | BTS7960 `LPWM` | Retract. GPT channel 0. |
| D6  | `OUTPUT`, PWM 490 Hz | BTS7960 `RPWM` | Extend. GPT channel 3, independent timer. |
| D7  | `INPUT_PULLUP` | Panel switch — settings AP | Open = enabled. Debounced 500 ms in software. |
| D8  | `OUTPUT` | DS3231 module — `VCC` | Carries the rail directly. Only legal because the module's power LED and charge resistor were removed (~0.8 mA vs a 4.0 mA pin). |
| D10 | `OUTPUT` (SD lib) | microSD — `CS` | |
| D11 | SPI COPI | microSD — `DI` | |
| D12 | SPI CIPO | microSD — `DO` | |
| D13 | SPI SCK | microSD — `CLK` | Also `LED_BUILTIN`. |
| A2  | `INPUT_PULLUP` | Panel switch — away override | Open = enabled. |
| A3  | Analog in, 10-bit | Anemometer — signal | 4.88 mV/count, 50 mV per m/s. |
| A4  | I²C SDA | DS3231 — `SDA` | |
| A5  | I²C SCL | DS3231 — `SCL` | |
| VIN | 6–24 V in | 12 V branch, fused | Buck regulator, not linear. |
| 5V  | Regulated out | SD `VCC`, BTS7960 `VCC` + `R_EN` + `L_EN`, hall `V+` | Logic only. |
| GND | Logic return | Star point | Motor return never passes through this pin. |

## Power

12 V PSU → main fuse → reverse-polarity stage → three fused branches:

| Branch | Feeds |
| --- | --- |
| Heavy | BTS7960 `B+` / `B−` → actuator |
| Light | Arduino `VIN` (time-delay fuse, not fast-blow) |
| Light | Anemometer (7–24 V part — never the 5 V rail) |

Ground is a star at the PSU negative, with exactly two returns: a heavy motor
return from the driver's power ground, and a light logic return collecting
Arduino GND, SD, RTC, anemometer, hall and the switch common.

**Never daisy-chain the motor return through the board's GND pin.** The
anemometer is referenced to the motor supply's ground; 100 mΩ of shared return
at 5 A is 0.5 V, which is 10 m/s of wind that is not there against a 24 m/s
park threshold.

Local to the driver: 1000 µF low-ESR + 100 nF across its supply terminals, and
a unidirectional TVS with cathode to +12 V.

## Gotchas

- **Panel switches are active-open.** `INPUT_PULLUP` + `== HIGH` means closing a
  contact to ground *disables* the feature. Switches on hand are SPDT — wire
  common to the pin, one throw to ground, spare throw to 5 V through 10 k.
- **`R_EN` and `L_EN` must be strapped to 5 V.** The BTS7960 holds both low
  through onboard 10 k pull-downs and the firmware drives neither. Floating
  enables mean the actuator never moves.
- **The Qwiic socket is `Wire1`**, a separate bus. The RTC goes on A4/A5.
- **Twist or shield the hall pair**, shield grounded at the board end only. PWM
  runs at 490 Hz — a 2.04 ms period — and the step counter only rejects edges
  closer than 500 µs, so coupled edges count as travel.
- **The SD module must be level-shifted and fed from 5 V.** `V_IH` on the RA4M1
  is 0.8 × VCC = 4.0 V, so an unbuffered 3.3 V MISO is below threshold.
- **Leave the ADC at default resolution.** The wind constant `0.004882814` is
  5 V / 1024, hard-wired to a 10-bit read.

## Parts

| Role | Part |
| --- | --- |
| Board | Arduino UNO R4 WiFi (ABX00087) |
| Motor driver | HiLetgo BTS7960 / IBT-2 |
| Supply | ALITOVE 12 V 30 A 360 W switching |
| RTC | Wishiot DS3231 + AT24C32, ZS-042 — LED and charge resistor removed |
| Storage | Stemedu microSD SPI module + SanDisk 32 GB High Endurance |
| Panel switches | 1P2T SPDT slide, 0.5 A 50 V |
| Terminations | ElectroCookie terminal shield, GeeekPi screw terminal hat |
| Actuator | **unidentified** |
| Anemometer | **unidentified** |

## Unmeasured

- Actuator stall current. Stall is the routine end-stop detector, and the 30 A
  supply will not current-limit out of a fault — the fuse and the motor wire are
  the only protection. Wire gauge and fuse rating both wait on this number.
- Hall harness: conductor count, supply voltage, output type.
- Whether the SD module actually carries an AMS1117-3.3 and a 74LVC125A. See
  the `SD.begin()` failures in `diagnostics/sd-card-test-2026-09-10.log`.
- Which DS3231 die is fitted. The seller ships `N`, `SN` or `M` at random; the
  `M` is ±5 ppm, not ±2.
