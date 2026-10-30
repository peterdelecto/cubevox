// Output mute sequencing, pure. MUTE_SW (low = muted) drives the DAC soft mute (XSMT) and the
// output relay (MUTE_N). Mute order is XSMT low, wait, MUTE_N low. Release order is XSMT high with
// the output held at digital zero, wait, MUTE_N high, wait for the relay, then the output fades up.
// The DAC therefore unmutes into an open relay and the contacts close on silence. The first release
// after power-on also waits until kRelayCloseAfterPowerOnMs after reset() before MUTE_N rises.
// The chain keeps running while muted. Timing constants live in core/timing.h.

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
  // Starts fully muted with no latch, hold or running clocks, and counts the power-on wait from
  // `nowMs`. `switchReleased` is the raw MUTE_SW level (high = not muted).
  void reset(bool switchReleased, uint32_t nowMs);

  // The SAI clocks run. XSMT may rise from here, into the open relay.
  void clocksRunning() { clocksRunning_ = true; }

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

  // True while the audio output must sit at digital zero: from the fully muted state until the relay
  // has closed on a release. It stays up through a mute that cuts a release short, and it comes down
  // when the sequence reaches Unmuted, where the output fades up. A mute from Unmuted leaves it down
  // until the sequence is fully muted again.
  bool outputHeld() const { return outputHeld_; }

 private:
  enum class State : uint8_t {
    Muted,      // XSMT low, MUTE_N low
    Unmuting,   // XSMT high, MUTE_N low: the DAC unmutes into the open relay
    Releasing,  // XSMT high, MUTE_N high: the relay closes
    Unmuted,    // both high
    Muting,     // XSMT low, MUTE_N high: waiting before MUTE_N goes low
  };

  void enter(State next, uint32_t nowMs) {
    state_ = next;
    sinceMs_ = nowMs;
    if (next == State::Unmuted) outputHeld_ = false;
    if (next == State::Muted || next == State::Unmuting) outputHeld_ = true;
  }
  bool unmuteWanted() const { return switch_.level() && clocksRunning_ && !latched_ && !held_; }
  bool muteWanted() const { return !switch_.level() || latched_ || held_; }
  bool relayMayClose(uint32_t nowMs) const { return nowMs - resetMs_ >= kRelayCloseAfterPowerOnMs; }
  MuteOutputs outputs() const;

  State state_ = State::Muted;
  uint32_t sinceMs_ = 0;
  uint32_t resetMs_ = 0;
  DebouncedInput switch_;
  bool clocksRunning_ = false;
  bool latched_ = false;
  bool held_ = false;
  bool tripped_ = false;
  bool outputHeld_ = true;
};
