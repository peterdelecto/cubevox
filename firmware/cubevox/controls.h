// Panel input: the 16-pot mux scan, per-pot smoothing, stepped knobs and the slot toggles.

#pragma once

#include <stdint.h>

#include "slots.h"

// Mux timing (hardware spec items 10 and 19). The 100 R / 1 nF at the ADC pin and the mux
// source impedance need >= 0.5 ms after a select change. The ADC sample time is a core
// compile-time default, set to 810.5 cycles (>= 27 us) by build.sh.
constexpr uint32_t kMuxSettleUs = 500;
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

struct LastMoved {
  int8_t slot = -1;  // -1 until a knob has moved
  bool knobB = false;
  uint32_t atMs = 0;
};

void controlsInit();

// Call every loop pass. Advances the mux scan without blocking and debounces the toggles.
void controlsPoll(uint32_t nowMs);

// True once since the last call if any knob, step or toggle changed (and once after the first full scan).
bool controlsTakeChanged();

// Applies every knob and toggle to `out`. Fields that belong to the menu are left alone.
void controlsApply(ChainParams& out);

// Current value of a slot knob as the OLED shows it (0..1, or the step index for stepped knobs).
float controlsKnobValue(int slot, bool knobB);

bool controlsToggleOn(int slot);

LastMoved controlsLastMoved();
