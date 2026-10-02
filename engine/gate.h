#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/smooth.h"

// Gate: one-pole highpassed peak detector drives a soft-knee gain computer with
// hysteresis, hold, and attack/release smoothing of the gain in dB.
// The input soft gate and the panel GATE are two instances of this class.

namespace cv {

struct GateTuning {
  float attackMs = 1.0f;
  float holdMs = 150.0f;
  float releaseMs = 20.0f;
  float rangeDb = -40.0f;       // attenuation when closed
  float kneeDb = 12.0f;         // soft knee width below the threshold
  float detectorHpHz = 120.0f;  // detector side-chain highpass
  float hysteresisDb = 6.0f;    // close level = open threshold - hysteresis
};

struct GateParams {
  bool on = true;
  float thresholdDb = -20.0f;  // panel knob -70..-10
  GateTuning tuning;
};

class Gate {
 public:
  void reset() {
    hpState_ = 0.0f;
    env_ = 0.0f;
    gainDb_ = 0.0f;
    onMix_ = 0.0f;
    holdLeft_ = 0;
    open_ = false;
  }

  void process(const float* in, float* out, int n, const GateParams& p) {
    const GateTuning& t = p.tuning;
    const float range = t.rangeDb < 0.0f ? t.rangeDb : 0.0f;
    const float knee = t.kneeDb > kKneeMinDb ? t.kneeDb : kKneeMinDb;
    const float hyst = t.hysteresisDb > 0.0f ? t.hysteresisDb : 0.0f;
    const float hpHz = t.detectorHpHz > kHpMinHz ? t.detectorHpHz : kHpMinHz;
    const float aHp = 1.0f - expf(-kTwoPi * hpHz / kSampleRate);
    const float aDet = smooth::coef(kDetectorReleaseSec);
    const float aAtt = smooth::coef(t.attackMs * 1e-3f);
    const float aRel = smooth::coef(t.releaseMs * 1e-3f);
    const float aOn = smooth::coef(smooth::kSmoothSec);
    const int holdSmp = static_cast<int>((t.holdMs > 0.0f ? t.holdMs : 0.0f) * kSampleRate / 1000.0f);
    const float openAt = p.thresholdDb;
    const float closeAt = p.thresholdDb - hyst;
    const float onTarget = p.on ? 1.0f : 0.0f;

    for (int i = 0; i < n; ++i) {
      // Detector.
      hpState_ += aHp * (in[i] - hpState_);
      const float mag = fabsf(in[i] - hpState_);
      env_ = mag > env_ ? mag : env_ + aDet * (mag - env_);
      const float levelDb = 20.0f * log10f(env_ + kFloor);

      // Gain computer.
      if (levelDb >= openAt) open_ = true;
      else if (levelDb < closeAt) open_ = false;
      float target = 0.0f;
      if (!open_) {
        const float below = openAt - levelDb;
        target = below >= knee ? range : range * below / knee;
      }

      // Attack, hold, release.
      if (target >= gainDb_) {
        holdLeft_ = holdSmp;
        gainDb_ = smooth::step(gainDb_, target, aAtt);
      } else if (holdLeft_ > 0) {
        --holdLeft_;
      } else {
        gainDb_ = smooth::step(gainDb_, target, aRel);
      }

      // Bypass fade: settled off multiplies by exactly 1.0.
      onMix_ = smooth::step(onMix_, onTarget, aOn);
      out[i] = in[i] * expf(gainDb_ * onMix_ * kDbToNeper);
    }
  }

  // Applied gain in dB, 0 while off.
  float gainDb() const { return gainDb_ * onMix_; }

 private:
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr float kDbToNeper = 0.11512925465f;
  static constexpr float kFloor = 1e-9f;
  static constexpr float kKneeMinDb = 0.1f;
  static constexpr float kHpMinHz = 1.0f;
  static constexpr float kDetectorReleaseSec = 0.020f;

  float hpState_ = 0.0f;
  float env_ = 0.0f;
  float gainDb_ = 0.0f;
  float onMix_ = 0.0f;
  int holdLeft_ = 0;
  bool open_ = false;
};

}  // namespace cv
