// Pure input logic: stepped positions and the per-knob filter.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "core/panel.h"
#include "core/slots.h"

// Dead band at each end of pot travel, as a fraction of the ADC range.
constexpr float kKnobEndMargin = 0.01f;

// Maps an ADC fraction (0..1) to a knob position (0..1). The dead band at each end reads as 0 and
// 1, so a pot that stops short of the rails still reaches both ends. NaN reads as 0.
float knobPosition(float adcFraction);

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

  // Re-derives the step position for a new step count without a new sample. None until seeded or
  // when the count is unchanged.
  KnobEvent restep(int steps);

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

// The panel's pots together: one filter each, plus the changed flag, the seeded count and the
// last moved knob the sketch needs.
class KnobBank {
 public:
  // Sets the step count per pot (0 = continuous). A seeded pot whose count changed re-derives its
  // position at once, so the next snapshot never mixes a new card with an old step index.
  void setSteps(const std::array<int, kPotCount>& steps);

  // Feeds one reading of `pot`. `owner.slot` is -1 for a pot no slot uses.
  void update(int pot, float position, LastMoved owner);

  float value(int pot) const { return knob_[static_cast<size_t>(pot)].value(); }

  // True once every pot has been read.
  bool seeded() const { return seededCount_ == kPotCount; }

  // True once since the last call if any knob moved, re-seeded, or the last pot was first read.
  bool takeChanged();

  LastMoved lastMoved() const { return lastMoved_; }

 private:
  std::array<KnobFilter, kPotCount> knob_;
  std::array<int, kPotCount> steps_{};
  int seededCount_ = 0;
  bool changed_ = false;
  LastMoved lastMoved_;
};
