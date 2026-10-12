#include "core/mute_sequence.h"

void MuteSequence::reset(bool switchReleased, uint32_t nowMs) {
  switch_.reset(switchReleased);
  enter(State::Muted, nowMs);
}

void MuteSequence::clocksRunning(uint32_t nowMs) {
  clocksRunning_ = true;
  clocksSinceMs_ = nowMs;
}

bool MuteSequence::unmuteWanted(uint32_t nowMs) const {
  const bool clocksReady = clocksRunning_ && nowMs - clocksSinceMs_ >= kBootUnmuteDelayMs;
  return switch_.level() && clocksReady && !latched_ && !held_;
}

MuteOutputs MuteSequence::outputs() const {
  switch (state_) {
    case State::Releasing: return {false, true};
    case State::Unmuted: return {true, true};
    case State::Muting: return {false, true};
    case State::Muted: break;
  }
  return {false, false};
}

MuteOutputs MuteSequence::update(uint32_t nowMs, bool switchReleased) {
  switch_.update(switchReleased, nowMs, kMuteSwitchDebounceMs);
  const uint32_t elapsed = nowMs - sinceMs_;

  switch (state_) {
    case State::Muted:
      if (unmuteWanted(nowMs)) enter(State::Releasing, nowMs);
      break;
    case State::Releasing:
      if (muteWanted()) {
        enter(State::Muting, nowMs);  // muted again mid-release: XSMT is still low
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
