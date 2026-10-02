#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/octave.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/shifters.h"

// Octave option B: the epoch shifter driven by the Octave panel.

namespace cv {

class OctaveVoiceB {
 public:
  static constexpr int kMaxSemis = 12;
  static constexpr float kMaxFormantSemis = 12.0f;

  OctaveVoiceB() { reset(); }

  void reset() { shifter_.reset(); }

  // Called once per block before ticking. Returns true if the stage is on;
  // a formant shift alone keeps it on at 0 semitones. corrOffset is Autotune's
  // correction in semitones.
  bool prepare(const PitchResult& pr, const OctaveParams& p, float corrOffset = 0.0f) {
    const int s = p.semitones < -kMaxSemis ? -kMaxSemis
                                           : (p.semitones > kMaxSemis ? kMaxSemis : p.semitones);
    const float fm = p.formant < -kMaxFormantSemis
                         ? -kMaxFormantSemis
                         : (p.formant > kMaxFormantSemis ? kMaxFormantSemis : p.formant);
    const OctaveTuning& t = p.tuning;
    const bool on = p.on && (s != 0 || fm != 0.0f);
    const float gain =
        (!on || (t.muteUnvoiced && !pr.voiced)) ? 0.0f : powf(10.0f, (t.levelDb + t.trimDbB) / 20.0f);
    shifter_.prepare(static_cast<float>(s) + corrOffset, fm, gain, t.glideMs, t.grainPeriods, t.epochSearch);
    return on;
  }

  float tick(const VoiceRing& ring, const VoiceRing& lp, long writeCount, float period) {
    return shifter_.tick(ring, lp, writeCount, period);
  }

 private:
  EpochShifter shifter_;
};

}  // namespace cv
