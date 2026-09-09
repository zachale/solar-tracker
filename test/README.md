# Tests

Native tests for the firmware's decision logic. The real `src/*.cpp` files are
compiled here against host stand-ins for the Arduino core, RTClib and SD, so
these exercise the shipping code rather than a copy of it.

```bash
./test/run.sh                 # everything
./test/run.sh -ts=schedule    # one suite
./test/run.sh -tc="*stall*"   # one case
```

Requires a host C++17 compiler and ArduinoJson, which the Arduino CLI already
installs to `~/Documents/Arduino/libraries`.

## Layout

| Path | Purpose |
| --- | --- |
| `run.sh` | Compiles firmware sources plus tests, then runs them |
| `doctest.h` | Test framework, single header, vendored at v2.4.11 |
| `stubs/Arduino.h` | String, pin I/O, and a clock the tests drive by hand |
| `stubs/RTClib.h` | DateTime arithmetic is real; the DS3231 is settable |
| `stubs/SD.h` | In-memory card |
| `stubs/mock.cpp` | Definitions for the mocked hardware state |

## Driving the mocks

`millis()` does not advance on its own. Tests move it, which is what makes the
dwell timers and the stall detector testable without waiting.

```cpp
mock::reset();                       // clock to 0, pins low, card empty
mock::advance(60001);                // move time forward
mock::analog[A3] = 900;              // wind sensor ADC reading
mock::digital[4] = HIGH;             // wind sensor enable switch
mock::sd_ok = false;                 // simulate a missing card
```

## Private methods

Several test files open with `#define private public`. The schedule
interpolation and the position arithmetic are private, and they are the parts
most worth pinning down. The alternative was widening the firmware's public
surface for the benefit of tests, which seemed worse. Access control affects
neither layout nor mangling, so the firmware translation units are unaffected.

## What is covered

- `ClockModule` — schedule interpolation across the whole day and across four
  different schedules, active-hours boundaries, the alarm flag
- `WindSpeedSensor` — voltage to speed, the enable switch, both dwell timers,
  and the gap between the park and release thresholds
- `LinearActuator` — percent/pulse conversion, the uncalibrated sentinel,
  stall detection and its step debounce
- `SettingsManager` — config and WiFi credential round-trips through the card

Not covered: `SolarTracker`, `WifiClient` and `SettingsServer`, which need
WiFiS3 and R4HttpClient stubs. `WifiClient::get()` is the one with a known live
defect, so it is the obvious next target.

## Known failure

`clock: alarm flag / the alarm flag reads true once, then clears` fails, and it
should. `ClockModule::isAlarmTriggered()` falls off the end of the function on
the path where the alarm *did* fire:

```cpp
bool ClockModule::isAlarmTriggered()
{
  if (!alarmTriggered) { return false; }
  alarmTriggered = false;
  resetAlarm();
}                                  // no return
```

That is undefined behaviour. On the Renesas target the caller reads whatever
`resetAlarm()` left in the return register — the byte count from its last
`Serial.println()`, which is non-zero, so the hourly move happens by accident.
A host compiler makes the same code return false, and the hourly move never
fires at all.

The fix is `return true;`. The compiler already finds it:

```
src/Clock/Clock.cpp:103:1: warning: non-void function does not return
  a value in all control paths [-Wreturn-type]
```

Arduino's default build does not show that warning. `run.sh` builds with
`-Wall -Wextra`, so it does.
