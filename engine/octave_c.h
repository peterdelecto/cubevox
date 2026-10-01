#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/octave.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// Octave option C: asynchronous granular shifter. N taps ride a shared
// sawtooth over a fixed window, Hann-crossfaded, so no tracker is involved
// and formants move with the pitch.

namespace cv {

class OctaveVoiceC {
 public:
  static constexpr int kMaxSemis = 12;
  static constexpr int kMaxGrains = 4;

  OctaveVoiceC() { reset(); }

  void reset() {
    phase_ = 0.0f;
    semis_ = 0.0f;
    gain_ = 0.0f;
    semisT_ = 0.0f;
    gainT_ = 0.0f;
    aGlide_ = 1.0f;
    windowSmp_ = OctaveTuning{}.grainWindowMs * kSmpPerMs;
    grains_ = OctaveTuning{}.grainCount;
    fresh_ = true;
  }

  // Called once per block before ticking. Returns true if the stage is on.
  // The tracker result is not used.
  bool prepare(const PitchResult&, const OctaveParams& p) {
    const int s = p.semitones < -kMaxSemis ? -kMaxSemis
                                           : (p.semitones > kMaxSemis ? kMaxSemis : p.semitones);
    const OctaveTuning& t = p.tuning;
    const bool on = p.on && s != 0;
    semisT_ = static_cast<float>(s);
    gainT_ = on ? powf(10.0f, (t.levelDb + t.trimDbC) / 20.0f) : 0.0f;
    // A silent voice jumps to its new interval instead of gliding in.
    if (fresh_ || gain_ == 0.0f) semis_ = semisT_;
    if (fresh_) gain_ = gainT_;
    fresh_ = false;
    aGlide_ = smooth::coef(t.glideMs * 0.001f);
    windowSmp_ = clampf(t.grainWindowMs, kMinWindowMs, kMaxWindowMs) * kSmpPerMs;
    grains_ = t.grainCount >= 4 ? 4 : 2;
    return on;
  }

  // One sample of the voice, gain and glide applied.
  float tick(const VoiceRing& ring, long writeCount) {
    gain_ = smooth::step(gain_, gainT_, smooth::coef(smooth::kSmoothSec));
    semis_ = smooth::step(semis_, semisT_, aGlide_);
    if (gain_ == 0.0f) return 0.0f;

    const int head = static_cast<int>(writeCount % kVoiceRingLen);
    const float ratio = exp2f(semis_ / 12.0f);
    const float d0 = 0.5f * windowSmp_ + kMinDelay;
    const float inv = 1.0f / static_cast<float>(grains_);

    // A tap at ratio r drifts (1 - r) samples of delay per sample; the
    // sawtooth is that drift normalised to the window.
    float sum = 0.0f;
    for (int k = 0; k < grains_; ++k) {
      const float p = frac(phase_ + static_cast<float>(k) * inv);
      const float g = 0.5f * (1.0f - cosf(kTwoPi * p));
      sum += g * readCubic(ring, head, d0 + windowSmp_ * (p - 0.5f));
    }
    phase_ = frac(phase_ + (1.0f - ratio) / windowSmp_);
    return gain_ * sum * 2.0f * inv;
  }

 private:
  static constexpr float kSmpPerMs = kSampleRate / 1000.0f;
  static constexpr float kMinWindowMs = 20.0f;
  static constexpr float kMaxWindowMs = 80.0f;
  static constexpr float kMinDelay = 4.0f;
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static_assert(kMaxWindowMs * kSmpPerMs + kMinDelay + 3.0f < kVoiceRingLen,
                "grains would outrun the ring");

  static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
  static float frac(float x) {
    x -= floorf(x);
    return x < 0.0f ? x + 1.0f : x;
  }

  static int wrap(int i) {
    i %= kVoiceRingLen;
    return i < 0 ? i + kVoiceRingLen : i;
  }

  // Catmull-Rom at a fractional delay behind the newest sample.
  static float readCubic(const VoiceRing& ring, int head, float delay) {
    const float pos = static_cast<float>(head) - delay;
    const float fl = floorf(pos);
    const float t = pos - fl;
    const int i0 = wrap(static_cast<int>(fl));
    const float xm = ring[wrap(i0 - 1)];
    const float x0 = ring[i0];
    const float x1 = ring[wrap(i0 + 1)];
    const float x2 = ring[wrap(i0 + 2)];
    const float c1 = 0.5f * (x1 - xm);
    const float c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return x0 + t * (c1 + t * (c2 + t * c3));
  }

  float phase_ = 0.0f;
  float semis_ = 0.0f;
  float gain_ = 0.0f;
  float semisT_ = 0.0f;
  float gainT_ = 0.0f;
  float aGlide_ = 1.0f;
  float windowSmp_ = 1920.0f;
  int grains_ = 2;
  bool fresh_ = true;
};

}  // namespace cv
