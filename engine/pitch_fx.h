#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/harmony.h"
#include "engine/octave.h"
#include "engine/octave_b.h"
#include "engine/octave_c.h"
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
    octaveA_.reset();
    octaveB_.reset();
    octaveC_.reset();
    lp_.fill(0.0f);
    lpZ_ = 0.0f;
    harmonyLp_.fill(0.0f);
    harmonyLpZ_ = 0.0f;
    engine_ = 0;
    mix_.fill(0.0f);
    active_.fill(0.0f);
    fresh_ = true;
  }

  void process(const float* in, float* out, int n, const PitchFxParams& p) {
    tracker_.push(in, n, p.harmony.tuning.voicedThreshold);
    const PitchResult& pr = tracker_.result();

    // Switching engines resets the one left behind so it starts clean next time.
    const int engine = p.octave.engine == 1 || p.octave.engine == 2 ? p.octave.engine : 0;
    if (engine != engine_) {
      if (engine != 0) octaveA_.reset();
      if (engine != 1) octaveB_.reset();
      if (engine != 2) octaveC_.reset();
      engine_ = engine;
    }
    const float lpA = lpCoef(p.octave.tuning.epochLpHz);
    const float harmonyLpA = lpCoef(p.harmony.tuning.shifter.epochLpHz);

    const std::array<float, kStages> mixT = {smooth::clamp01(p.harmony.mix),
                                             smooth::clamp01(p.octave.mix)};
    const std::array<float, kStages> activeT = {harmony_.prepare(pr, p.harmony) ? 1.0f : 0.0f,
                                                (prepareOctave(pr, p.octave))
                                                    ? 1.0f
                                                    : 0.0f};
    if (fresh_) {
      mix_ = mixT;
      active_ = activeT;
      fresh_ = false;
    }

    const float a = smooth::coef(smooth::kSmoothSec);
    for (int i = 0; i < n; ++i) {
      ring_[writeCount_] = in[i];
      lpZ_ += lpA * (in[i] - lpZ_);
      lp_[writeCount_] = lpZ_;
      harmonyLpZ_ += harmonyLpA * (in[i] - harmonyLpZ_);
      harmonyLp_[writeCount_] = harmonyLpZ_;
      const float harm = harmony_.tick(ring_, harmonyLp_, writeCount_, pr.period);
      const float oct = tickOctave(pr.period);
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
  // One-pole coefficient for an option B peak-search low-pass.
  static float lpCoef(float hz) {
    hz = hz < kMinLpHz ? kMinLpHz : (hz > kMaxLpHz ? kMaxLpHz : hz);
    return 1.0f - expf(-kTwoPi * hz / static_cast<float>(kSampleRate));
  }

  bool prepareOctave(const PitchResult& pr, const OctaveParams& o) {
    if (engine_ == 1) return octaveB_.prepare(pr, o);
    if (engine_ == 2) return octaveC_.prepare(pr, o);
    return octaveA_.prepare(pr, o);
  }

  float tickOctave(float period) {
    if (engine_ == 1) return octaveB_.tick(ring_, lp_, writeCount_, period);
    if (engine_ == 2) return octaveC_.tick(ring_, writeCount_);
    return octaveA_.tick(ring_, writeCount_, period);
  }

  static constexpr int kStages = 2;  // 0 harmony, 1 octave
  static constexpr float kHalfPi = 1.57079632679489661923f;
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr float kMinLpHz = 100.0f;
  static constexpr float kMaxLpHz = 4000.0f;

  VoiceRing ring_{};
  PitchTracker tracker_;
  HarmonyVoices harmony_;
  OctaveVoice octaveA_;
  OctaveVoiceB octaveB_;
  OctaveVoiceC octaveC_;
  VoiceRing lp_{};  // ring_ low-passed for Octave B's peak search
  float lpZ_ = 0.0f;
  VoiceRing harmonyLp_{};  // the same at Harmony's own cutoff, for Harmony B
  float harmonyLpZ_ = 0.0f;
  int engine_ = 0;
  std::array<float, kStages> mix_{};
  std::array<float, kStages> active_{};
  long writeCount_ = 0;
  bool fresh_ = true;
};

}  // namespace cv
