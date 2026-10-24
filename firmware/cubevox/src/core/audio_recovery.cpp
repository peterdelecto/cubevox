#include "core/audio_recovery.h"

// The slot the next restart overwrites holds the oldest of the last kRestartsPerWindow restarts.
bool StreamRecovery::budgetLeft(uint32_t nowMs) const {
  if (restarts_ < static_cast<uint32_t>(kRestartsPerWindow)) return true;
  return nowMs - restartMs_[restarts_ % kRestartsPerWindow] >= kRestartWindowMs;
}

StreamRecovery::Action StreamRecovery::update(uint32_t nowMs, bool fault, bool fullyMuted) {
  switch (state_) {
    case State::Running:
      if (!fault) return Action::None;
      if (!budgetLeft(nowMs)) {
        state_ = State::Latched;
        return Action::Latch;
      }
      state_ = State::Waiting;
      [[fallthrough]];
    case State::Waiting:
      if (!fullyMuted) return Action::None;
      restartMs_[restarts_ % kRestartsPerWindow] = nowMs;
      ++restarts_;
      state_ = State::Running;
      return Action::Restart;
    case State::Latched: break;
  }
  return Action::None;
}
