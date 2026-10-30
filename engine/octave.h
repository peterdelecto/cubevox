#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// Octave: one voice a fixed number of semitones from the input, by an asynchronous granular
// shifter. Its taps ride a shared sawtooth over a fixed window and are Hann-crossfaded, so no pitch
// tracker is involved and the formants move with the pitch. PitchFx owns the ring and the mix. The
// voice's output passes a 5 Hz DC blocker before the mix, so the dry path is untouched.

namespace cv {

struct OctaveTuning {
  float levelDb = 0.0f;
  float glideMs = 15.0f;        // interval glide
  float grainWindowMs = 40.0f;  // 20..80
  int grainCount = 2;           // taps, 2 or 4
  float trimDb = 2.2f;          // on top of levelDb; level rule at MIX 1 (wet only)
};

struct OctaveParams {
  bool on = true;      // panel toggle; off fades the stage out
  int semitones = 12;  // panel knob, -12..12, 0 = off
  float mix = 0.15f;   // panel knob
  OctaveTuning tuning;
};

class OctaveVoice {
 public:
  static constexpr int kMaxSemis = 12;
  static constexpr int kMaxGrains = 4;

  OctaveVoice() { reset(); }

  void reset() {
    phase_ = 0.0f;
    semis_ = 0.0f;
    gain_ = 0.0f;
    semisT_ = 0.0f;
    gainT_ = 0.0f;
    aGlide_ = 1.0f;
    ratio_ = 1.0f;
    ratioSemis_ = 0.0f;
    windowSmp_ = OctaveTuning{}.grainWindowMs * kSmpPerMs;
    grains_ = OctaveTuning{}.grainCount;
    dc_.clear();
    fresh_ = true;
  }

  // Called once per block before ticking. Returns true if the stage is on. corrOffset is
  // Autotune's correction in semitones.
  bool prepare(const OctaveParams& p, float corrOffset) {
    const int s = p.semitones < -kMaxSemis ? -kMaxSemis : (p.semitones > kMaxSemis ? kMaxSemis : p.semitones);
    const OctaveTuning& t = p.tuning;
    const bool on = p.on && s != 0;
    semisT_ = static_cast<float>(s) + corrOffset;
    gainT_ = on ? powf(10.0f, (t.levelDb + t.trimDb) / 20.0f) : 0.0f;
    // Smoothers start on target after reset; a silent voice jumps to its new interval instead of
    // gliding in from a stale one.
    if (fresh_ || gain_ == 0.0f) semis_ = semisT_;
    if (fresh_) gain_ = gainT_;
    fresh_ = false;
    aGlide_ = smooth::coef(t.glideMs * 0.001f);
    windowSmp_ = clampf(t.grainWindowMs, kMinWindowMs, kMaxWindowMs) * kSmpPerMs;
    grains_ = t.grainCount >= kMaxGrains ? kMaxGrains : 2;
    syncWindows();
    return on;
  }

  // One sample of the voice, gain and glide applied, then a DC blocker at kWetDcBlockHz. A tap at
  // ratio r drifts (1 - r) samples of delay per sample; the sawtooth is that drift normalised to
  // the window. A silent voice (gain 0) clears the blocker, so it returns exactly 0.
  float tick(const VoiceRing& ring, long writeCount) {
    gain_ = smooth::step(gain_, gainT_, smooth::kSmoothCoef);
    semis_ = smooth::step(semis_, semisT_, aGlide_);
    if (gain_ == 0.0f) {
      dc_.clear();
      return 0.0f;
    }
    updateRatio();
    const int head = static_cast<int>(writeCount);
    const float d0 = 0.5f * windowSmp_ + kMinDelay;
    const float inv = 1.0f / static_cast<float>(grains_);
    float sum = 0.0f;
    for (int k = 0; k < grains_; ++k) {
      const float p = frac(phase_ + static_cast<float>(k) * inv);
      HannRotor& w = window_[static_cast<size_t>(k)];
      const float g = w.window();
      w.advance();
      sum += g * readCubic(ring, head, d0 + windowSmp_ * (p - 0.5f));
    }
    phase_ = frac(phase_ + (1.0f - ratio_) / windowSmp_);
    return dc_.run(gain_ * sum * 2.0f * inv);
  }

 private:
  static constexpr float kSmpPerMs = kSampleRate / 1000.0f;
  static constexpr float kMinWindowMs = 20.0f;
  static constexpr float kMaxWindowMs = 80.0f;
  static constexpr float kMinDelay = 4.0f;
  static_assert(kMaxWindowMs * kSmpPerMs + kMinDelay + 3.0f < kVoiceRingLen, "grains would outrun the ring");

  static float frac(float x) {
    x -= floorf(x);
    return x < 0.0f ? x + 1.0f : x;
  }

  // exp2 of semis_ / 12, recomputed only while the interval glides.
  void updateRatio() {
    if (semis_ == ratioSemis_) return;
    ratio_ = exp2Fast(semis_ / 12.0f);
    ratioSemis_ = semis_;
  }

  // Each tap's Hann rotor restarts from the exact phase every block. The step uses this block's
  // ratio, so a glide is off by a hair until the next block syncs it again.
  void syncWindows() {
    updateRatio();
    const float inv = 1.0f / static_cast<float>(grains_);
    const float step = kTwoPi * (1.0f - ratio_) / windowSmp_;
    for (int k = 0; k < grains_; ++k)
      window_[static_cast<size_t>(k)].init(kTwoPi * frac(phase_ + static_cast<float>(k) * inv), step);
  }

  static float readCubic(const VoiceRing& ring, int head, float delay) {
    return readRing<kVoiceRingLen>(ring.data(), head, delay);
  }

  float phase_ = 0.0f;
  float semis_ = 0.0f;
  float gain_ = 0.0f;
  float semisT_ = 0.0f;
  float gainT_ = 0.0f;
  float aGlide_ = 1.0f;
  float ratio_ = 1.0f;
  float ratioSemis_ = 0.0f;
  float windowSmp_ = 1920.0f;
  int grains_ = 2;
  std::array<HannRotor, kMaxGrains> window_{};
  DcBlocker dc_;
  bool fresh_ = true;
};

}  // namespace cv
