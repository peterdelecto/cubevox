#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/harmony.h"
#include "engine/octave.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// Shared pitch front end. Harmony and Octave both track and shift the dry
// input, so neither tracker ever hears the other's voices. Each stage has its
// own equal-power MIX; dry gain is the product of the two.

namespace cv {

struct PitchFxParams {
  HarmonyParams harmony;
  OctaveParams octave;
};

class PitchFx {
 public:
  PitchFx() { reset(); }

  void reset() {
    ring_.fill(0.0f);
    writeCount_ = 0;
    tracker_.reset();
    harmony_.reset();
    octave_.reset();
    mix_.fill(0.0f);
    active_.fill(0.0f);
    fresh_ = true;
  }

  void process(const float* in, float* out, int n, const PitchFxParams& p) {
    tracker_.push(in, n, p.harmony.tuning.voicedThreshold);
    const PitchResult& pr = tracker_.result();

    const std::array<float, kStages> mixT = {smooth::clamp01(p.harmony.mix),
                                             smooth::clamp01(p.octave.mix)};
    const std::array<float, kStages> activeT = {harmony_.prepare(pr, p.harmony) ? 1.0f : 0.0f,
                                                octave_.prepare(pr, p.octave) ? 1.0f : 0.0f};
    if (fresh_) {
      mix_ = mixT;
      active_ = activeT;
      fresh_ = false;
    }

    const float a = smooth::coef(smooth::kSmoothSec);
    for (int i = 0; i < n; ++i) {
      ring_[writeCount_] = in[i];
      const float harm = harmony_.tick(ring_, writeCount_, pr.period);
      const float oct = octave_.tick(ring_, writeCount_, pr.period);
      float dry = 1.0f;
      std::array<float, kStages> wet{};
      for (int s = 0; s < kStages; ++s) {
        mix_[s] = smooth::step(mix_[s], mixT[s], a);
        active_[s] = smooth::step(active_[s], activeT[s], a);
        dry *= 1.0f + (cosf(mix_[s] * kHalfPi) - 1.0f) * active_[s];
        wet[s] = sinf(mix_[s] * kHalfPi) * active_[s];
      }
      out[i] = dry * in[i] + wet[0] * harm + wet[1] * oct;
      // Voices only use writeCount modulo the ring, so wrapping here is exact.
      writeCount_ = writeCount_ + 1 == kVoiceRingLen ? 0 : writeCount_ + 1;
    }
  }

  const PitchResult& pitch() const { return tracker_.result(); }

 private:
  static constexpr int kStages = 2;  // 0 harmony, 1 octave
  static constexpr float kHalfPi = 1.57079632679489661923f;

  VoiceRing ring_{};
  PitchTracker tracker_;
  HarmonyVoices harmony_;
  OctaveVoice octave_;
  std::array<float, kStages> mix_{};
  std::array<float, kStages> active_{};
  long writeCount_ = 0;
  bool fresh_ = true;
};

}  // namespace cv
