#include "Watchdog.h"
#include <WDT.h>
#include "FspTimer.h"

namespace
{
// Longest silence from the main loop before the supervisor stops refreshing. It has to
// cover the slowest legal blocking call, an OTA startDownload at 60 s, plus a WiFi
// association and a request retry, with room to spare.
const unsigned long DEADLINE_MS = 180000UL;
// Asked for 5 s; the part grants 5,592 ms, its maximum.
const uint32_t WDT_REQUEST_MS = 5000;

FspTimer supervisor;
volatile unsigned long lastProgress = 0;
// Set only once the hardware watchdog is running. Until then the supervisor must not touch
// it: in register-start mode a refresh is what starts the countdown.
volatile bool running = false;
uint8_t cause = 0;

// Runs at 1 Hz. Refreshing from an interrupt would normally hide a hang, but the refresh is
// conditional on the main loop having moved, and a hard fault runs above this interrupt's
// priority, so a faulted chip stops the refreshes and is reset within 5.6 s.
void tick(timer_callback_args_t *)
{
  if (running && millis() - lastProgress <= DEADLINE_MS)
  {
    WDT.refresh();
  }
}

// The RA4M1 latches why it last reset and nothing in the core reads or clears the flags.
// Clearing them here makes the next boot's reading about the next reset only.
void latchResetCause()
{
  if (R_SYSTEM->RSTSR0_b.PORF)
    cause |= Watchdog::RESET_POWER_ON;
  if (R_SYSTEM->RSTSR1_b.WDTRF)
    cause |= Watchdog::RESET_WATCHDOG;
  if (R_SYSTEM->RSTSR1_b.SWRF)
    cause |= Watchdog::RESET_SOFTWARE;

  R_SYSTEM->PRCR = (uint16_t)0xA502U; // unlock PRC1, as variant.cpp does
  R_SYSTEM->RSTSR0 = 0;
  R_SYSTEM->RSTSR1 = 0;
  R_SYSTEM->PRCR = (uint16_t)0xA500U;
}
} // namespace

bool Watchdog::begin()
{
  latchResetCause();
  lastProgress = millis();

  // The supervisor first. Starting the hardware watchdog is irreversible, and one with
  // nothing refreshing it resets the chip every 5.6 s, before setup() ever reaches the OTA
  // bookkeeping that could roll a bad image back. So the watchdog is only started once the
  // thing that feeds it is known to be running.
  //
  // force = false on purpose: the variant marks the GPT channels behind the PWM pins as
  // taken, so the actuator keeps them. AGT0 drives millis() but is advertised as free
  // (FspTimer.cpp leaves the line that would reserve it commented out), so it is refused.
  uint8_t type = GPT_TIMER;
  int8_t channel = FspTimer::get_available_timer(type);
  if (channel < 0 || (type == AGT_TIMER && channel == 0))
  {
    Serial.println("WATCHDOG NOT ARMED: no safe timer channel");
    return false;
  }
  if (!(supervisor.begin(TIMER_MODE_PERIODIC, type, (uint8_t)channel, 1.0f, 0.0f, tick) &&
        supervisor.setup_overflow_irq() && supervisor.open() && supervisor.start()))
  {
    Serial.println("WATCHDOG NOT ARMED: the supervisor timer would not start");
    return false;
  }

  // WDT.begin() opens the peripheral and refreshes it once, which starts it.
  if (WDT.begin(WDT_REQUEST_MS) == 0)
  {
    Serial.println("WATCHDOG NOT ARMED: the WDT refused the period");
    return false;
  }
  running = true;

  Serial.println("Watchdog armed: " + String(WDT.getTimeout()) + " ms hardware, " + String(DEADLINE_MS / 1000) +
                 " s loop deadline, " + String(type == GPT_TIMER ? "GPT" : "AGT") + String(channel) +
                 ", last reset: " + resetCauseText());
  return true;
}

String Watchdog::describe()
{
  if (!running)
  {
    return "WATCHDOG NOT ARMED, last reset: " + resetCauseText();
  }
  return "Watchdog armed: " + String(WDT.getTimeout()) + " ms hardware, " + String(DEADLINE_MS / 1000) +
         " s loop deadline, last reset: " + resetCauseText();
}

bool Watchdog::armed()
{
  return running;
}

void Watchdog::progress()
{
  lastProgress = millis();
}

uint8_t Watchdog::resetCause()
{
  return cause;
}

String Watchdog::resetCauseText()
{
  if (cause & RESET_WATCHDOG)
    return "watchdog";
  if (cause & RESET_SOFTWARE)
    return "software";
  if (cause & RESET_POWER_ON)
    return "power on";
  return "reset pin";
}
