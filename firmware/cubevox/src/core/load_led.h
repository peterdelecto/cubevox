// The user LED shows the audio load as brightness. The box drives the LED with PWM and the emulator
// draws the same level, both from loadLedLevel().
//
// Level 0 is dark and 1 is full. The level follows the square of the mean load's share of the
// layout limit, so it is dark with audio stopped, a faint glow at no load and full at the limit. A
// PWM duty is linear in light, and the eye reads light on roughly a square-root scale, so a duty
// that rises with the square of the load looks like an even climb. A late, dry or overrun block
// holds the LED full for kLoadLedFlashMs.

#pragma once

#include <stdint.h>

#include "core/costs.h"

// The mean load at which the LED is full: the average line a layout is gated against.
constexpr int kLoadLedLimitPercent = costs::kBudgetAvgPercent;

// The level at zero load, a duty of 3 %, so a running box with a light chain still glows.
constexpr float kLoadLedIdle = 0.03f;

// How long a late, dry or overrun block holds the LED full.
constexpr uint32_t kLoadLedFlashMs = 100;

// The time since the last trouble when there has been none.
constexpr uint32_t kLoadLedNoTrouble = UINT32_MAX;

// The LED level, 0 to 1. `meanPercent` is the mean block time as a percent of the block budget and
// `limitPercent` the load that reads full. `audioLive` is false while audio is stopped, stalled or
// latched, and the LED is dark whatever else is true. `msSinceTrouble` is the time since the last
// late, dry or overrun block, or kLoadLedNoTrouble.
inline float loadLedLevel(int meanPercent, int limitPercent, bool audioLive, uint32_t msSinceTrouble) {
  if (!audioLive) return 0.0f;
  if (msSinceTrouble < kLoadLedFlashMs) return 1.0f;
  float share = 1.0f;
  if (limitPercent > 0) {
    share = static_cast<float>(meanPercent) / static_cast<float>(limitPercent);
    share = share < 0.0f ? 0.0f : share > 1.0f ? 1.0f : share;
  }
  return kLoadLedIdle + (1.0f - kLoadLedIdle) * share * share;
}

// Turns a running count of late, dry and overrun blocks into the time since it last changed. The
// first count only sets the baseline, so blocks counted before the first pass do not flash.
class BlockTroubleClock {
 public:
  void note(uint32_t count, uint32_t nowMs) {
    if (seen_ && count != count_) {
      lastMs_ = nowMs;
      flagged_ = true;
    }
    count_ = count;
    seen_ = true;
  }

  // The time since the count last changed, or kLoadLedNoTrouble if it never has.
  uint32_t sinceMs(uint32_t nowMs) const { return flagged_ ? nowMs - lastMs_ : kLoadLedNoTrouble; }

 private:
  uint32_t count_ = 0;
  uint32_t lastMs_ = 0;
  bool seen_ = false;
  bool flagged_ = false;
};
