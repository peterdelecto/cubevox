#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/octave.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/shifters.h"

// Octave option C: the granular shifter driven by the Octave panel.

namespace cv {

class OctaveVoiceC {
 public:
  static constexpr int kMaxSemis = 12;
  static constexpr int kMaxGrains = GrainShifter::kMaxGrains;

  OctaveVoiceC() { reset(); }

  void reset() { shifter_.reset(); }

  // Called once per block before ticking. Returns true if the stage is on.
  // The tracker result is not used; corrOffset is Autotune's correction in
  // semitones.
  bool prepare(const PitchResult&, const OctaveParams& p, float corrOffset = 0.0f) {
    const int s = p.semitones < -kMaxSemis ? -kMaxSemis
                                           : (p.semitones > kMaxSemis ? kMaxSemis : p.semitones);
    const OctaveTuning& t = p.tuning;
    const bool on = p.on && s != 0;
    const float gain = on ? powf(10.0f, (t.levelDb + t.trimDbC) / 20.0f) : 0.0f;
    shifter_.prepare(static_cast<float>(s) + corrOffset, gain, t.glideMs, t.grainWindowMs, t.grainCount);
    return on;
  }

  float tick(const VoiceRing& ring, long writeCount) { return shifter_.tick(ring, writeCount); }

 private:
  GrainShifter shifter_;
};

}  // namespace cv
