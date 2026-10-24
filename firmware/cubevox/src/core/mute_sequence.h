// Output mute sequencing, pure. MUTE_SW (low = muted) drives the DAC soft mute (XSMT) and the
// output relay (MUTE_N). Mute order is XSMT low, wait, MUTE_N low. Release order is MUTE_N high,
// wait, XSMT high. The chain keeps running while muted. Timing constants live in core/timing.h.

#pragma once

#include <stdint.h>

#include "core/debounce.h"
#include "core/timing.h"

// Pin levels the board drives; true = high.
struct MuteOutputs {
  bool xsmtHigh = false;
  bool muteNHigh = false;
};

class MuteSequence {
 public:
  // Starts fully muted with no latch, hold or running clocks. `switchReleased` is the raw MUTE_SW
  // level (high = not muted).
  void reset(bool switchReleased, uint32_t nowMs);

  // The box stays muted until kBootUnmuteDelayMs after the SAI clocks started.
  void clocksRunning(uint32_t nowMs);

  // Mutes and ignores MUTE_SW until reset.
  void latch() { latched_ = true; }

  // Another reason to stay muted. Releasing it runs the normal release sequence.
  void hold(bool on) { held_ = on; }

  // An interrupt already drove both pins low by register (power fail, audio trip). The sequence is
  // muted from the next update on and stays there while the trip lasts; then it releases as usual.
  void trip(bool on) { tripped_ = on; }

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
  bool tripped_ = false;
};
