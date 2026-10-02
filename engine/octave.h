#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/shifters.h"
#include "engine/smooth.h"

// Octave: one voice a fixed number of semitones from the sung pitch.
// PitchFx owns the ring, the tracker, and the mix.

namespace cv {

struct OctaveTuning {
  float levelDb = 0.0f;
  float glideMs = 15.0f;
  bool muteUnvoiced = false;

  // Option B
  float grainPeriods = ShifterTuning{}.grainPeriods;  // grain length in sung periods, 1.5..3
  float epochSearch = ShifterTuning{}.epochSearch;    // peak search half-width, fraction of a period
  float epochLpHz = ShifterTuning{}.epochLpHz;        // low-pass used to find the peak

  // Per-engine output trims; level rule at mix 0.5, on top of levelDb.
  float trimDbA = -5.3f;
  float trimDbB = 6.2f;

  // Option C
  float grainWindowMs = ShifterTuning{}.grainWindowMs;  // 20..80
  int grainCount = ShifterTuning{}.grainCount;          // 2 or 4
  float trimDbC = 2.6f;
};

struct OctaveParams {
  bool on = true;                // panel toggle; off fades the stage out
  int engine = 0;                // 0 = A (grid PSOLA), 1 = B (epoch PSOLA + formant), 2 = C (granular)
  int semitones = 0;             // panel knob, -12..12, 0 = off
  float formant = 0.0f;          // option B, -12..12 semitones, 0 = preserved
  float mix = 0.5f;              // panel knob
  OctaveTuning tuning;
};

class OctaveVoice {
 public:
  static constexpr int kMaxSemis = 12;

  OctaveVoice() { reset(); }

  void reset() {
    voice_.reset();
    semis_ = 0.0f;
    gain_ = 0.0f;
    semisT_ = 0.0f;
    gainT_ = 0.0f;
    aGlide_ = 1.0f;
    fresh_ = true;
  }

  // Called once per block before ticking. Returns true if the stage is on.
  // corrOffset is Autotune's correction in semitones.
  bool prepare(const PitchResult& pr, const OctaveParams& p, float corrOffset = 0.0f) {
    const int s = p.semitones < -kMaxSemis ? -kMaxSemis
                                           : (p.semitones > kMaxSemis ? kMaxSemis : p.semitones);
    const OctaveTuning& t = p.tuning;
    const bool on = p.on && s != 0;
    semisT_ = static_cast<float>(s) + corrOffset;
    gainT_ = (!on || (t.muteUnvoiced && !pr.voiced)) ? 0.0f : powf(10.0f, (t.levelDb + t.trimDbA) / 20.0f);
    // Smoothers start on target after reset; a silent voice jumps to its new
    // interval instead of gliding in from a stale one.
    if (fresh_ || gain_ == 0.0f) semis_ = semisT_;
    if (fresh_) gain_ = gainT_;
    fresh_ = false;
    aGlide_ = smooth::coef(t.glideMs * 0.001f);
    return on;
  }

  // One sample of the voice, gain and glide applied.
  float tick(const VoiceRing& ring, long writeCount, float period) {
    gain_ = smooth::step(gain_, gainT_, smooth::coef(smooth::kSmoothSec));
    semis_ = smooth::step(semis_, semisT_, aGlide_);
    if (gain_ == 0.0f) return 0.0f;
    return gain_ * voice_.tick(ring, writeCount, period, exp2f(semis_ / 12.0f));
  }

 private:
  PsolaVoice voice_;
  float semis_ = 0.0f;
  float gain_ = 0.0f;
  float semisT_ = 0.0f;
  float gainT_ = 0.0f;
  float aGlide_ = 1.0f;
  bool fresh_ = true;
};

}  // namespace cv
