#include "core/jack_switch.h"

void JackSwitch::update(uint32_t nowMs, InputJack requested, bool outputFullyMuted) {
  if (state_ == State::Idle && requested != relay_) state_ = State::WaitMute;

  if (state_ == State::WaitMute && outputFullyMuted) {
    relay_ = requested;
    switchedAtMs_ = nowMs;
    state_ = State::Settling;
  }

  if (state_ == State::Settling && nowMs - switchedAtMs_ >= kRelaySettleMs) {
    state_ = requested == relay_ ? State::Idle : State::WaitMute;
  }
}
