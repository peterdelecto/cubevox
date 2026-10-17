#include "core/debounce.h"

bool DebouncedInput::update(bool raw, uint32_t nowMs, uint32_t holdMs) {
  if (raw != candidate_) {
    candidate_ = raw;
    candidateSinceMs_ = nowMs;
    return false;
  }
  if (candidate_ == stable_ || nowMs - candidateSinceMs_ < holdMs) return false;
  stable_ = candidate_;
  return true;
}
