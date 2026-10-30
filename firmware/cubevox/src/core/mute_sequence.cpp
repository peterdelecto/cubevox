#include "core/mute_sequence.h"

void MuteSequence::reset(bool switchReleased, uint32_t nowMs) {
  switch_.reset(switchReleased);
  resetMs_ = nowMs;
  clocksRunning_ = false;
  latched_ = false;
  held_ = false;
  tripped_ = false;
  enter(State::Muted, nowMs);
}

MuteOutputs MuteSequence::outputs() const {
  switch (state_) {
    case State::Unmuting: return {true, false};
    case State::Releasing: return {true, true};
    case State::Unmuted: return {true, true};
    case State::Muting: return {false, true};
    case State::Muted: break;
  }
  return {false, false};
}

MuteOutputs MuteSequence::update(uint32_t nowMs, bool switchReleased) {
  switch_.update(switchReleased, nowMs, kSwitchDebounceMs);
  if (tripped_) {
    if (state_ != State::Muted) enter(State::Muted, nowMs);  // the pins are low already
    return outputs();
  }
  const uint32_t elapsed = nowMs - sinceMs_;

  switch (state_) {
    case State::Muted:
      if (unmuteWanted()) enter(State::Unmuting, nowMs);
      break;
    case State::Unmuting:
      if (muteWanted()) {
        enter(State::Muted, nowMs);  // MUTE_N is still low, so dropping XSMT again is silent
      } else if (elapsed >= kXsmtLeadMs && relayMayClose(nowMs)) {
        enter(State::Releasing, nowMs);
      }
      break;
    case State::Releasing:
      if (muteWanted()) {
        enter(State::Muting, nowMs);  // muted again mid-release: XSMT drops, then MUTE_N
      } else if (elapsed >= kRelaySettleMs) {
        enter(State::Unmuted, nowMs);
      }
      break;
    case State::Unmuted:
      if (muteWanted()) enter(State::Muting, nowMs);
      break;
    case State::Muting:
      if (elapsed >= kXsmtLeadMs) enter(State::Muted, nowMs);
      break;
  }
  return outputs();
}
