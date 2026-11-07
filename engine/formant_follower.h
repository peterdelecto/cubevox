#pragma once

#include <cmath>

#include "engine/dsp.h"
#include "engine/formant_params.h"
#include "engine/syllable_sweep.h"

// Apraxia: the throat drops on every syllable. The card moves the Formant voice's SHIFT with the singing,
// the way an envelope-filter pedal moves its filter with the pluck. The pitch stays where it is. The card
// drives the Formant engine with its MIX pinned at 1, so the output is the shifted voice alone at the
// Formant card's trim.
//
// The syllable sweep (`syllable_sweep.h`) reads each block of the card's input, the block the slot ahead
// hands it. SHIFT is RANGE times the sweep. RANGE runs from -12 st at the pot's bottom through 0 at noon
// to +12 st at its top, so a sweep moves the throat down below noon and up above it, the same reach each way.
//
// The Formant engine takes SHIFT in whole semitones and glides it through its own 20 ms one-pole, the only
// smoothing SHIFT gets. Each call hands it the wanted SHIFT plus what the last call's rounding left over,
// rounded. The whole semitones then average to the wanted SHIFT, and the engine's glide smooths their
// steps.

namespace cv {

constexpr float kFollowerMinRangeSt = -12.0f;  // RANGE at the pot's bottom
constexpr float kFollowerMaxRangeSt = 12.0f;   // RANGE at the pot's top; noon is 0, no effect
constexpr float kFollowerDefaultRangeSt = -5.0f;

static_assert(kFollowerMinRangeSt >= -static_cast<float>(kFormantVoiceMaxShiftSt) &&
                  kFollowerMaxRangeSt <= static_cast<float>(kFormantVoiceMaxShiftSt),
              "RANGE stays inside the voice's limit");

constexpr float kFollowerDefaultSensitivity = kSweepDefaultSensitivity;
constexpr float kFollowerMinDecayMs = kSweepMinDecayMs;
constexpr float kFollowerMaxDecayMs = kSweepMaxDecayMs;
constexpr float kFollowerDefaultDecayMs = kSweepDefaultDecayMs;

struct FormantFollowerTuning {
  float decayMs = kFollowerDefaultDecayMs;  // a sweep falls from the drive to 0 over this
};

struct FormantFollowerParams {
  bool placed = false;                              // the card is in the layout, so its sweep sets the Formant voice's SHIFT
  float sensitivity = kFollowerDefaultSensitivity;  // knob A (SENSITIVITY), 0..1
  float rangeSt = kFollowerDefaultRangeSt;          // knob B (RANGE), -12 to +12 st
  FormantFollowerTuning tuning;
};

class FormantFollower {
 public:
  void reset() {
    sweep_.reset();
    residue_ = 0.0f;
    last_ = 0.0f;
  }

  // The Formant engine's parameters for one call of n samples from `in`, the input of the slot that runs
  // it. With the card placed they are `p` with the voice's SHIFT taken from the sweep and its MIX at 1;
  // otherwise they are `p` itself.
  const FormantParams& follow(const float* in, int n, const FormantParams& p, const FormantFollowerParams& f) {
    if (!f.placed) return p;
    params_ = p;
    params_.shiftSt = shiftFor(in, n, f);
    params_.mix = 1.0f;
    return params_;
  }

  // Steps the follower over n samples from `in` and returns this call's SHIFT in whole semitones.
  int shiftFor(const float* in, int n, const FormantFollowerParams& f) {
    if (n <= 0) return static_cast<int>(last_);
    const float sweep = sweep_.step(in, n, f.sensitivity, f.tuning.decayMs);

    const float range = clampf(f.rangeSt, kFollowerMinRangeSt, kFollowerMaxRangeSt);
    const float want = range * sweep + residue_;
    if (!(want > kFollowerMinRangeSt - 1.0f && want < kFollowerMaxRangeSt + 1.0f)) {
      residue_ = 0.0f;
      last_ = 0.0f;
      return 0;
    }
    last_ = clampf(floorf(want + 0.5f), kFollowerMinRangeSt, kFollowerMaxRangeSt);
    residue_ = clampf(want - last_, -0.5f, 0.5f);
    return static_cast<int>(last_);
  }

  // The follower's peak level, linear, for the tests.
  float level() const { return sweep_.level(); }

  // The drive, 0 to 1, for the tests and the render tool.
  float drive() const { return sweep_.drive(); }

  // The sweep, 0 to 1; SHIFT is RANGE times it.
  float sweep() const { return sweep_.sweep(); }

 private:
  SyllableSweep sweep_;
  float residue_ = 0.0f;  // what the last call's rounding left over, in semitones
  float last_ = 0.0f;     // the last SHIFT handed out
  FormantParams params_;  // the copy follow() hands the engine
};

}  // namespace cv
