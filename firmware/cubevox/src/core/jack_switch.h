// Sequencing policy for the 1/4" INPUT relay K103. The relay may only switch while the output
// is fully muted, so the level step and the contact click never reach the output.

#pragma once

#include <stdint.h>

#include "core/settings.h"
#include "core/timing.h"

class JackSwitch {
 public:
  // The relay starts on `initial`; at boot the output is already muted, so no dip is needed.
  explicit JackSwitch(InputJack initial) : relay_(initial) {}

  // Call every loop pass with the latest request and whether the output is fully muted.
  void update(uint32_t nowMs, InputJack requested, bool outputFullyMuted);

  // True while the output must stay muted for a relay change.
  bool holdMute() const { return state_ != State::Idle; }

  // The state PAD_EN should be driven to.
  InputJack relay() const { return relay_; }

 private:
  enum class State : uint8_t { Idle, WaitMute, Settling };

  State state_ = State::Idle;
  InputJack relay_;
  uint32_t switchedAtMs_ = 0;
};
