#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/smooth.h"

// Slapback: one delayed repeat of the voice through a 2-pole lowpass.
// INTENSITY sets the wet level. Time, cutoff and feedback are tuning items.

namespace cv {

struct SlapbackTuning {
  float timeMs = 70.0f;       // 30..120
  float lowpassHz = 4000.0f;  // 500..12000
  float feedback = 0.35f;     // 0..0.5; ~2.5 audible repeats (3rd at about -20 dB)
  float wetMaxDb = 4.9f;      // wet level at INTENSITY 1; level rule at 0.5
};

struct SlapbackParams {
  bool on = true;
  float intensity = 0.0f;  // panel knob 0..1
  SlapbackTuning tuning;
};

class Slapback {
 public:
  void reset() {
    line_.fill(0.0f);
    z1_ = 0.0f;
    z2_ = 0.0f;
    intensity_ = 0.0f;
    timeSmp_ = 0.0f;
    timeValid_ = false;
    writePos_ = 0;
  }

  void process(const float* in, float* out, int n, const SlapbackParams& p) {
    const SlapbackTuning& t = p.tuning;
    const float target = p.on ? smooth::clamp01(p.intensity) : 0.0f;
    const float wetMax = powf(10.0f, t.wetMaxDb / 20.0f);
    const float timeTarget = clampf(t.timeMs, kTimeMinMs, kTimeMaxMs) * kSmpPerMs;
    const float feedback = clampf(t.feedback, 0.0f, kFeedbackMax);
    const float aInt = smooth::coef(kIntensitySec);
    const float aTime = smooth::coef(kTimeSec);
    setLowpass(t.lowpassHz);

    // The first block after reset lands on the tuned time with no glide.
    if (!timeValid_) {
      timeSmp_ = timeTarget;
      timeValid_ = true;
    }

    for (int i = 0; i < n; ++i) {
      intensity_ = smooth::step(intensity_, target, aInt);
      timeSmp_ += aTime * (timeTarget - timeSmp_);

      const float lp = filter(readCubic(timeSmp_));
      line_[writePos_] = in[i] + feedback * lp;
      out[i] = in[i] + intensity_ * wetMax * lp;
      writePos_ = (writePos_ + 1) % kLen;
    }
  }

 private:
  static constexpr int kLen = 130 * kSampleRate / 1000;
  static constexpr float kSmpPerMs = kSampleRate / 1000.0f;
  static constexpr float kTimeMinMs = 30.0f;
  static constexpr float kTimeMaxMs = 120.0f;
  static constexpr float kCutoffMinHz = 500.0f;
  static constexpr float kCutoffMaxHz = 12000.0f;
  static constexpr float kFeedbackMax = 0.5f;
  static constexpr float kQ = 0.70710678f;
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr float kIntensitySec = 0.020f;
  static constexpr float kTimeSec = 0.050f;

  static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

  // RBJ lowpass, recomputed once per block.
  void setLowpass(float hz) {
    const float w0 = kTwoPi * clampf(hz, kCutoffMinHz, kCutoffMaxHz) / kSampleRate;
    const float cw = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * kQ);
    const float a0 = 1.0f + alpha;
    b0_ = 0.5f * (1.0f - cw) / a0;
    b1_ = (1.0f - cw) / a0;
    a1_ = -2.0f * cw / a0;
    a2_ = (1.0f - alpha) / a0;
  }

  // Transposed direct form II.
  float filter(float x) {
    const float y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b0_ * x - a2_ * y;
    return y;
  }

  static int wrap(int i) {
    if (i < 0) return i + kLen;
    if (i >= kLen) return i - kLen;
    return i;
  }

  // Catmull-Rom through four taps. Delay is clamped so the newest tap
  // never crosses the write head and the oldest never wraps onto it.
  float readCubic(float delay) const {
    const float d = clampf(delay, 2.0f, kLen - 3.0f);
    const float pos = static_cast<float>(writePos_) - d;
    const float fl = floorf(pos);
    const float f = pos - fl;
    const int i0 = wrap(static_cast<int>(fl) % kLen);
    const float xm = line_[wrap(i0 - 1)];
    const float x0 = line_[i0];
    const float x1 = line_[wrap(i0 + 1)];
    const float x2 = line_[wrap(i0 + 2)];
    const float c1 = 0.5f * (x1 - xm);
    const float c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return x0 + f * (c1 + f * (c2 + f * c3));
  }

  std::array<float, kLen> line_{};
  float b0_ = 1.0f, b1_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
  float z2_ = 0.0f, z1_ = 0.0f;
  float intensity_ = 0.0f;
  float timeSmp_ = 0.0f;
  bool timeValid_ = false;
  int writePos_ = 0;
};

}  // namespace cv
