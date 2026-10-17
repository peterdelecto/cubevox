// Switch input that only changes after the raw level has held for the debounce time.

#pragma once

#include <stdint.h>

class DebouncedInput {
 public:
  void reset(bool level) {
    stable_ = level;
    candidate_ = level;
  }

  // Returns true when the stable level changed on this call.
  bool update(bool raw, uint32_t nowMs, uint32_t holdMs);
  bool level() const { return stable_; }

 private:
  bool stable_ = false;
  bool candidate_ = false;
  uint32_t candidateSinceMs_ = 0;
};
