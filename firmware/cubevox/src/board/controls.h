// Panel input: the 16-pot mux scan, per-pot smoothing, stepped knobs and the slot toggles.

#pragma once

#include <stdint.h>

#include "core/layout.h"
#include "core/panel.h"

// Mux timing (hardware spec items 10 and 19). The 100 R / 1 nF at the ADC pin and the mux
// source impedance need >= 0.5 ms after a select change. The ADC sample time is a core
// compile-time default, set to 810.5 cycles (>= 27 us) by build.sh.
constexpr uint32_t kMuxSettleUs = 500;
// U107 holds 3V3A off for up to 1030 ms after power-on; a high select before that pushes
// ~20 mA through U101's input clamp into the dead rail. Selects stay low until this passes.
constexpr uint32_t kMuxSelectHoldMs = 1100;
constexpr uint32_t kToggleDebounceMs = 20;

// Switch input that only changes after the raw level has held for the debounce time.
class DebouncedInput {
 public:
  void reset(bool level) {
    stable_ = level;
    candidate_ = level;
  }
  // Returns true when the stable level changed on this call.
  bool update(bool raw, uint32_t nowMs, uint32_t holdMs);
  bool level() const { return stable_; }

 private:
  bool stable_ = false;
  bool candidate_ = false;
  uint32_t candidateSinceMs_ = 0;
};

// Maps a 0..1 knob position onto `positions` equal bands. The knob must travel
// `hysteresis` of a band past an edge before the position changes, so a value resting on
// an edge does not flicker. `current` is the previous position.
int steppedPosition(float norm, int positions, int current, float hysteresis = 0.2f);

void controlsInit();

// Call every loop pass. Advances the mux scan without blocking and debounces the toggles.
// Step counts come from `layout`; a knob whose count changes re-seeds its step position.
void controlsPoll(uint32_t nowMs, const Layout& layout);

// True once since the last call if any knob, step or toggle changed (and once after the first full scan).
bool controlsTakeChanged();

// Knob values (0..1, or the step index for stepped knobs), toggles and the last moved knob.
PanelReading controlsReading();
