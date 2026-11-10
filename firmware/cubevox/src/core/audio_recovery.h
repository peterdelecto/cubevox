// When the audio stream restarts after a fault, pure. A fault holds the mute, the stream restarts
// once the output is fully muted, and the mute releases after the usual settle. At most
// kRestartsPerWindow restarts fit in any kRestartWindowMs. A fault past that, or a restart that
// fails, resets the box onto the power-on path (owner 2026-11-10).

#pragma once

#include <stdint.h>

#include <array>

constexpr int kRestartsPerWindow = 3;
constexpr uint32_t kRestartWindowMs = 60000;

class StreamRecovery {
 public:
  enum class Action : uint8_t {
    None,
    Restart,  // restart the stream now, then clear the fault
    Reset,    // reset the box; nothing restarts after this
  };

  // Call every loop pass. `fault` is true while a fault waits for its restart; `fullyMuted` is
  // whether the output is muted.
  Action update(uint32_t nowMs, bool fault, bool fullyMuted);

  // The restart did not bring the stream back. Returns Reset; nothing restarts after this.
  Action fail() {
    state_ = State::Resetting;
    return Action::Reset;
  }

  // True from a fault until its restart, so the caller holds the mute.
  bool holdMute() const { return state_ == State::Waiting; }

  // Restarts since boot.
  uint32_t restarts() const { return restarts_; }

 private:
  enum class State : uint8_t { Running, Waiting, Resetting };

  bool budgetLeft(uint32_t nowMs) const;

  std::array<uint32_t, kRestartsPerWindow> restartMs_{};  // ring of the last restarts
  uint32_t restarts_ = 0;
  State state_ = State::Running;
};
