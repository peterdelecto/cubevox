// The user LED shows the audio load. Each half-second period the LED is lit for the mean load's
// share of it, so 10 % gives a 50 ms blip, 50 % gives half on and half off, and 90 % reads near
// solid with a 50 ms dark gap. The LED stays dark while audio is not running.

#pragma once

#include <stdint.h>

constexpr uint32_t kLoadLedPeriodMs = 500;

// True when the LED is lit at `nowMs`. `meanPercent` is held to 0..100.
inline bool loadLedLit(uint32_t nowMs, int meanPercent, bool audioRunning) {
  const uint32_t held = meanPercent < 0 ? 0u : meanPercent > 100 ? 100u : static_cast<uint32_t>(meanPercent);
  return audioRunning && (nowMs % kLoadLedPeriodMs) * 100 < kLoadLedPeriodMs * held;
}
