#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/harmony.h"
#include "engine/octave.h"
#include "engine/octave_b.h"
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
    lp_.fill(0.0f);
    lpZ_ = 0.0f;
    useB_ = false;
    mix_.fill(0.0f);
    active_.fill(0.0f);
    fresh_ = true;
  }

  void process(const float* in, float* out, int n, const PitchFxParams& p) {
    tracker_.push(in, n, p.harmony.tuning.voicedThreshold);
    const PitchResult& pr = tracker_.result();

    // Switching engines resets the one left behind so it starts clean next time.
    const bool useB = p.octave.engine == 1;
    if (useB != useB_) {
      if (useB) {
        octaveA_.reset();
      } else {
        octaveB_.reset();
      }
      useB_ = useB;
    }
    float lpHz = p.octave.tuning.epochLpHz;
    lpHz = lpHz < kMinLpHz ? kMinLpHz : (lpHz > kMaxLpHz ? kMaxLpHz : lpHz);
    const float lpA = 1.0f - expf(-kTwoPi * lpHz / static_cast<float>(kSampleRate));

    const std::array<float, kStages> mixT = {smooth::clamp01(p.harmony.mix),
                                             smooth::clamp01(p.octave.mix)};
    const std::array<float, kStages> activeT = {harmony_.prepare(pr, p.harmony) ? 1.0f : 0.0f,
                                                (useB ? octaveB_.prepare(pr, p.octave)
                                                      : octaveA_.prepare(pr, p.octave))
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
      const float harm = harmony_.tick(ring_, writeCount_, pr.period);
      const float oct = useB ? octaveB_.tick(ring_, lp_, writeCount_, pr.period)
                             : octaveA_.tick(ring_, writeCount_, pr.period);
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
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr float kMinLpHz = 100.0f;
  static constexpr float kMaxLpHz = 4000.0f;

  VoiceRing ring_{};
  PitchTracker tracker_;
  HarmonyVoices harmony_;
  OctaveVoice octaveA_;
  OctaveVoiceB octaveB_;
  VoiceRing lp_{};  // ring_ low-passed for option B's peak search
  float lpZ_ = 0.0f;
  bool useB_ = false;
  std::array<float, kStages> mix_{};
  std::array<float, kStages> active_{};
  long writeCount_ = 0;
  bool fresh_ = true;
};

}  // namespace cv
