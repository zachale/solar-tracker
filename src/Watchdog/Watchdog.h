#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <Arduino.h>

// Two watchdogs in one, after docs/prd/04-watchdog-and-recovery.md.
//
// The RA4M1's hardware WDT tops out at 5,592 ms on this part, shorter than a single legal
// AT command (10 s) and far shorter than an OTA startDownload (up to 60 s). So a 1 Hz timer
// interrupt refreshes the hardware watchdog, and it only does so while the main loop has
// reported progress within the deadline. A slow call cannot reset the board; a loop that
// never comes back, or a hard fault that stops interrupts, cannot avoid it.
//
// Arm it first thing in setup(). It is off after every reset until then: the part is in
// register-start mode, so a board sitting in its bootloader is never reset by it.
class Watchdog
{
public:
  enum ResetCause : uint8_t
  {
    RESET_POWER_ON = 1,
    RESET_WATCHDOG = 2,
    RESET_SOFTWARE = 4,
  };

  // Latches why the chip last reset, then arms the watchdog. False means nothing is armed.
  static bool begin();
  // Whether begin() succeeded. OTA refuses to start a trial from, and rolls back to, an
  // image without one: nothing else turns a hang into the reset that a rollback needs.
  static bool armed();
  // "The main loop advanced." Call only after a bounded call has returned.
  static void progress();
  static uint8_t resetCause();
  // One line on the watchdog's state, for printing once a serial capture can hear it.
  static String describe();
  static String resetCauseText();
};

#endif
