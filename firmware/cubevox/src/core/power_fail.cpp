#include "core/power_fail.h"

bool PowerFailWatch::update(uint32_t nowMs, uint32_t railMv, bool interruptTripped) {
  if (interruptTripped || railMv < kPowerTripMv) {
    tripped_ = true;
    good_ = false;  // the hold starts over at the next good reading
    return true;
  }
  if (railMv < kPowerClearMv) {
    good_ = false;
    return tripped_;
  }
  if (!good_) {
    good_ = true;
    goodSinceMs_ = nowMs;
  }
  if (tripped_ && nowMs - goodSinceMs_ >= kPowerHoldMs) tripped_ = false;
  return tripped_;
}
