// Power-fail mute policy, pure (hardware spec F2 and F3). VA_SENSE is 5VA through 10 k / 10 k
// (hardware spec item 13), read against VREF+ at 3.3 V. The board trips the mute in hardware the
// moment 5VA falls below kPowerTripMv; this policy decides when the rail is good again.

#pragma once

#include <stdint.h>

constexpr uint32_t kVrefMv = 3300;
constexpr uint32_t kVaSenseDivider = 2;

// Trip level, spec F2. Provisional: the bench sets the final value against the rails' fall time.
constexpr uint32_t kPowerTripMv = 4750;

// The rail must climb this far above the trip level, so noise at the line cannot chatter the mute.
constexpr uint32_t kPowerClearMv = 4850;

// The rail must hold at or above kPowerClearMv this long before the mute releases.
constexpr uint32_t kPowerHoldMs = 500;

// ADC reading at `bits` resolution for a 5VA level, rounded down.
constexpr uint32_t railCounts(uint32_t railMv, int bits) {
  return railMv * ((1u << bits) - 1u) / (kVaSenseDivider * kVrefMv);
}

// 5VA in mV for an ADC reading at `bits` resolution, rounded down.
constexpr uint32_t railMv(uint32_t counts, int bits) {
  return counts * kVaSenseDivider * kVrefMv / ((1u << bits) - 1u);
}

class PowerFailWatch {
 public:
  // Call every loop pass with 5VA in mV and whether the trip interrupt fired since the last call.
  // A reading under the trip level trips as well, so a missed interrupt still mutes. Returns true
  // while the mute must hold for the rail.
  bool update(uint32_t nowMs, uint32_t railMv, bool interruptTripped);

  bool tripped() const { return tripped_; }

 private:
  bool tripped_ = false;
  bool good_ = false;  // at or above kPowerClearMv since goodSinceMs_
  uint32_t goodSinceMs_ = 0;
};
