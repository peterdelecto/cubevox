#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/smooth.h"

// Slapback: one delayed repeat of the voice through a 2-pole lowpass.
// MIX sets the wet level and the repeats together; TIME is the second
// panel knob. Repeats land on the tuned feedback at MIX 25 % and grow as
// sqrt(MIX), capped at kFeedbackMax.

namespace cv {

struct SlapbackTuning {
  float timeMs = 100.0f;      // panel TIME, 60..250
  float lowpassHz = 4000.0f;  // 500..12000
  float feedback = 0.35f;     // at MIX 25 %: ~2.5 audible repeats (3rd at about -20 dB)
  float wetMaxDb = 4.9f;      // wet level at MIX 1
  float trimDb = -4.9f;       // whole output, scaled by MIX; level rule at 0.5
};

struct SlapbackParams {
  bool on = true;
  float mix = 0.0f;  // panel MIX 0..1
  SlapbackTuning tuning;
};

class Slapback {
 public:
  void reset() {
    line_.fill(0.0f);
    lp_.clear();
    mix_ = 0.0f;
    timeSmp_ = 0.0f;
    timeValid_ = false;
    writePos_ = 0;
  }

  void process(const float* in, float* out, int n, const SlapbackParams& p) {
    const SlapbackTuning& t = p.tuning;
    const float target = p.on ? smooth::clamp01(p.mix) : 0.0f;
    const float timeTarget = clampf(t.timeMs, kTimeMinMs, kTimeMaxMs) * kSmpPerMs;
    const float aTime = smooth::coef(kTimeSec);
    lp_.setLowPass(clampf(t.lowpassHz, kCutoffMinHz, kCutoffMaxHz), kButterworthQ);

    // The first block after reset lands on the tuned time with no glide.
    if (!timeValid_) {
      timeSmp_ = timeTarget;
      timeValid_ = true;
    }

    // MIX steps per sample; wet gain, feedback and trim follow it, so they are evaluated at the
    // block edges and interpolated between.
    float mixEnd = mix_;
    for (int i = 0; i < n; ++i) mixEnd = smooth::step(mixEnd, target, smooth::kSmoothCoef);
    const Edge e0 = edgeAt(t, mix_);
    const Edge e1 = edgeAt(t, mixEnd);
    const float inv = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      const float f = static_cast<float>(i + 1) * inv;
      timeSmp_ += aTime * (timeTarget - timeSmp_);
      const float echo = lp_.run(readCubic(timeSmp_));
      const float feedback = e0.feedback + (e1.feedback - e0.feedback) * f;
      const float wetGain = e0.wet + (e1.wet - e0.wet) * f;
      const float trim = e0.trim + (e1.trim - e0.trim) * f;
      line_[static_cast<size_t>(writePos_)] = in[i] + feedback * echo;
      out[i] = (in[i] + wetGain * echo) * trim;
      writePos_ = writePos_ + 1 == kLen ? 0 : writePos_ + 1;
    }
    mix_ = mixEnd;
  }

 private:
  static constexpr int kLen = 260 * kSampleRate / 1000;
  static constexpr float kSmpPerMs = kSampleRate / 1000.0f;
  static constexpr float kTimeMinMs = 60.0f;
  static constexpr float kTimeMaxMs = 250.0f;
  static constexpr float kCutoffMinHz = 500.0f;
  static constexpr float kCutoffMaxHz = 12000.0f;
  static constexpr float kFeedbackMax = 0.5f;
  static constexpr float kFeedbackRefMix = 0.25f;  // MIX where the tuned feedback applies
  static constexpr float kTimeSec = 0.050f;

  // Everything that follows MIX, at one MIX value.
  struct Edge {
    float wet;       // wet gain
    float feedback;  // repeats
    float trim;      // whole-output trim
  };

  static Edge edgeAt(const SlapbackTuning& t, float mix) {
    const float feedbackRef = clampf(t.feedback, 0.0f, kFeedbackMax);
    return {mix * powf(10.0f, t.wetMaxDb / 20.0f),
            fminf(kFeedbackMax, feedbackRef * sqrtf(mix / kFeedbackRefMix)),
            powf(10.0f, t.trimDb * mix / 20.0f)};
  }

  // Catmull-Rom through four taps. Delay is clamped so the newest tap
  // never crosses the write head and the oldest never wraps onto it.
  float readCubic(float delay) const {
    return readRing<kLen>(line_.data(), writePos_, clampf(delay, 2.0f, kLen - 3.0f));
  }

  std::array<float, kLen> line_{};
  Biquad lp_;  // repeat low-pass, set once per block
  float mix_ = 0.0f;
  float timeSmp_ = 0.0f;
  bool timeValid_ = false;
  int writePos_ = 0;
};

}  // namespace cv
