#include "mute.h"

#include <Arduino.h>

#include "controls.h"
#include "pins.h"

namespace {

enum class State : uint8_t {
  Muted,       // XSMT low, MUTE_N low
  Releasing,   // MUTE_N high, waiting for the relay before XSMT goes high
  Unmuted,     // both high
  Muting,      // XSMT low, waiting before MUTE_N goes low
};

constexpr uint32_t kSwitchDebounceMs = 20;

State gState = State::Muted;
uint32_t gStateSinceMs = 0;
DebouncedInput gSwitch;
bool gClocksRunning = false;
uint32_t gClocksSinceMs = 0;

void enter(State next, uint32_t nowMs) {
  gState = next;
  gStateSinceMs = nowMs;
}

// True when the toggle asks for sound and the boot delay has passed.
bool unmuteWanted(uint32_t nowMs) {
  const bool switchReleased = gSwitch.level();  // high = not muted
  const bool clocksReady = gClocksRunning && nowMs - gClocksSinceMs >= kBootUnmuteDelayMs;
  return switchReleased && clocksReady;
}

}  // namespace

void muteInit() {
  digitalWrite(pins::kMuteN, LOW);
  pinMode(pins::kMuteN, OUTPUT);
  digitalWrite(pins::kXsmt, LOW);
  pinMode(pins::kXsmt, OUTPUT);
  pinMode(pins::kMuteSw, INPUT);  // R120 pull-up on the board
  gSwitch.reset(digitalRead(pins::kMuteSw) == HIGH);
  enter(State::Muted, millis());
}

void muteClocksRunning(uint32_t nowMs) {
  gClocksRunning = true;
  gClocksSinceMs = nowMs;
}

void mutePoll(uint32_t nowMs) {
  gSwitch.update(digitalRead(pins::kMuteSw) == HIGH, nowMs, kSwitchDebounceMs);
  const uint32_t elapsed = nowMs - gStateSinceMs;

  switch (gState) {
    case State::Muted:
      if (unmuteWanted(nowMs)) {
        digitalWrite(pins::kMuteN, HIGH);
        enter(State::Releasing, nowMs);
      }
      break;
    case State::Releasing:
      if (!gSwitch.level()) {
        enter(State::Muting, nowMs);  // pressed again mid-release: XSMT is still low
        digitalWrite(pins::kXsmt, LOW);
      } else if (elapsed >= kRelaySettleMs) {
        digitalWrite(pins::kXsmt, HIGH);
        enter(State::Unmuted, nowMs);
      }
      break;
    case State::Unmuted:
      if (!gSwitch.level()) {
        digitalWrite(pins::kXsmt, LOW);
        enter(State::Muting, nowMs);
      }
      break;
    case State::Muting:
      if (elapsed >= kXsmtLeadMs) {
        digitalWrite(pins::kMuteN, LOW);
        enter(State::Muted, nowMs);
      }
      break;
  }
}

bool muteActive() { return gState != State::Unmuted; }
