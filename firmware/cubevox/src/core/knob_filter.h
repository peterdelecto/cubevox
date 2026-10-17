// Pure input logic: stepped positions and the per-knob filter.

#pragma once

#include <stdint.h>

// Maps a 0..1 knob position onto `positions` equal bands. The knob must travel
// `hysteresis` of a band past an edge before the position changes, so a value resting on
// an edge does not flicker. `current` is the previous position. Returns 0 when `positions`
// is not positive.
int steppedPosition(float norm, int positions, int current, float hysteresis = 0.2f);

enum class KnobEvent : uint8_t {
  None,      // nothing to report
  Seeded,    // first sample taken
  Reseeded,  // step count changed; position re-derived from the knob
  Moved,     // knob travelled past the threshold or crossed to another step
};

// Smooths one pot and reports when it moved. `steps` is the knob's step count, 0 for a
// continuous knob.
class KnobFilter {
 public:
  KnobEvent update(float norm, int steps);

  // Step index for a stepped knob, smoothed 0..1 otherwise.
  float value() const { return stepsSeeded_ > 0 ? static_cast<float>(step_) : smoothed_; }

 private:
  void reseed(int steps);

  float smoothed_ = 0.0f;
  float reported_ = 0.0f;  // value at the last Moved event
  int step_ = 0;
  int stepsSeeded_ = 0;    // step count step_ was seeded for; 0 for a continuous knob
  bool seeded_ = false;
};
