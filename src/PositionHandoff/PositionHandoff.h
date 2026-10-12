#ifndef POSITION_HANDOFF_H
#define POSITION_HANDOFF_H

#include <Arduino.h>
#include "../LinearActuator/LinearActuator.h"

// Carries the actuator's position across an OTA restart, so the new firmware starts where
// the old one stopped instead of sweeping the panel through a full calibration.
//
// The file is written right before the restart and read once, then deleted, by the next
// boot. It is only believed by the version it was written for, and only within ten minutes
// of being written (by the tracker's clock): an install that failed leaves the old
// firmware running, and its next reset (or a power cut, or a watchdog reset, or a boot that
// could not read the card) finds another version, an old file or none, and calibrates as
// it always has.
// It has a file of its own, never the settings file, which is rewritten from scratch.
class PositionHandoff
{
public:
  static bool save(const char *version, const LinearActuator::Snapshot &s, uint32_t now);
  static bool take(const char *version, uint32_t now, LinearActuator::Snapshot &s);

private:
  static constexpr const char *FILE_NAME = "POS.TXT";
  static const uint32_t MAX_AGE_S = 600;
};

#endif
