// Dims the OLED after a long idle spell. A knob move, a slot toggle flip, a change of the top switch,
// a MENU turn or press, or a voice at the input counts as activity. The screen dims once the last
// activity is kDimAfterMs old, and the next activity brings it back at once. The code is pure, so
// the host tests drive it with a fake clock.

#pragma once

#include <stdint.h>

#include "core/brightness.h"
#include "core/level.h"

// The screen dims when this long has passed since the last activity.
constexpr uint32_t kDimAfterMs = 600000;

class ScreenDim {
 public:
  // Power-on counts as activity.
  void start(uint32_t nowMs) { noteActivity(nowMs); }

  // A knob moved, a slot toggle flipped, or the MENU was turned or pressed.
  void noteActivity(uint32_t nowMs) {
    lastActivityMs_ = nowMs;
    dimmed_ = false;
  }

  // The top switch's position, once per pass. A change of position is activity. The first call
  // only records the position.
  void noteTopSwitch(bool engaged, uint32_t nowMs) {
    if (topSwitchKnown_ && engaged != topSwitchEngaged_) noteActivity(nowMs);
    topSwitchEngaged_ = engaged;
    topSwitchKnown_ = true;
  }

  // The input energy of one UI tick, the same figure the input meter reads. A tick above the meter's
  // floor counts as activity.
  void noteVoice(const level::InputEnergy& energy, uint32_t nowMs) {
    if (level::toDbfs(level::rms(energy)) > level::kFloorDbfs) noteActivity(nowMs);
  }

  // The contrast the panel should hold now. Once the screen has dimmed it stays dim until activity,
  // even if the millisecond counter laps the last activity.
  uint8_t contrast(uint32_t nowMs, Brightness brightness) {
    if (nowMs - lastActivityMs_ >= kDimAfterMs) dimmed_ = true;
    return dimmed_ ? kDimmedContrast : brightnessLevel(brightness).contrast;
  }

 private:
  uint32_t lastActivityMs_ = 0;
  bool dimmed_ = false;
  bool topSwitchKnown_ = false;
  bool topSwitchEngaged_ = false;
};
