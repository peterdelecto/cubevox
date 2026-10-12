// Output mute sequencing, pure. MUTE_SW (low = muted) drives the DAC soft mute (XSMT) and the
// output relay (MUTE_N). Mute order is XSMT low, wait, MUTE_N low. Release order is MUTE_N high,
// wait, XSMT high. The chain keeps running while muted.

#pragma once

#include <stdint.h>

#include "core/jack_switch.h"  // kRelaySettleMs
#include "core/knob_filter.h"

// Timing in ms. The XSMT lead is spec note F3 (>= 3.4 ms at 48 kHz) with margin.
// The relay settle (kRelaySettleMs) covers the G6K-2F-Y operate time. The boot delay is spec note F2.
constexpr uint32_t kXsmtLeadMs = 5;
constexpr uint32_t kBootUnmuteDelayMs = 500;
constexpr uint32_t kMuteSwitchDebounceMs = 20;

// Pin levels the board drives; true = high.
struct MuteOutputs {
  bool xsmtHigh = false;
  bool muteNHigh = false;
  bool operator==(const MuteOutputs& o) const { return xsmtHigh == o.xsmtHigh && muteNHigh == o.muteNHigh; }
  bool operator!=(const MuteOutputs& o) const { return !(*this == o); }
};

class MuteSequence {
 public:
  // Starts fully muted. `switchReleased` is the raw MUTE_SW level (high = not muted).
  void reset(bool switchReleased, uint32_t nowMs);

  // The box stays muted until kBootUnmuteDelayMs after the SAI clocks started.
  void clocksRunning(uint32_t nowMs);

  // Mutes and ignores MUTE_SW until reset.
  void latch() { latched_ = true; }

  // Another reason to stay muted. Releasing it runs the normal release sequence.
  void hold(bool on) { held_ = on; }

  // Call every loop pass with the raw MUTE_SW level. Returns the pin levels to drive.
  MuteOutputs update(uint32_t nowMs, bool switchReleased);

  // True while muted or while a mute / release sequence is in progress.
  bool active() const { return state_ != State::Unmuted; }

  // True once MUTE_N has gone low (XSMT went low first). K102 is still opening for kRelaySettleMs
  // after this, and XSMT keeps the output silent meanwhile.
  bool fullyMuted() const { return state_ == State::Muted; }

 private:
  enum class State : uint8_t {
    Muted,      // XSMT low, MUTE_N low
    Releasing,  // MUTE_N high, waiting for the relay before XSMT goes high
    Unmuted,    // both high
    Muting,     // XSMT low, waiting before MUTE_N goes low
  };

  void enter(State next, uint32_t nowMs) {
    state_ = next;
    sinceMs_ = nowMs;
  }
  bool unmuteWanted(uint32_t nowMs) const;
  bool muteWanted() const { return !switch_.level() || latched_ || held_; }
  MuteOutputs outputs() const;

  State state_ = State::Muted;
  uint32_t sinceMs_ = 0;
  DebouncedInput switch_;
  bool clocksRunning_ = false;
  uint32_t clocksSinceMs_ = 0;
  bool latched_ = false;
  bool held_ = false;
};
