#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Gate: one-pole highpassed peak detector drives a soft-knee gain computer with
// hysteresis, hold, and attack/release smoothing of the gain in dB.
// The input soft gate and the panel GATE are two instances of this class.
// The per-sample dB conversions run through fastmath. fastmath_test pins both to
// libm, and gate_test checks the applied gain against it.

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
    hp_.clear();
    env_ = 0.0f;
    gainDb_ = 0.0f;
    onMix_ = 0.0f;
    holdLeft_ = 0;
    open_ = false;
  }

  void process(const float* in, float* out, int n, const GateParams& p) {
    const Law law = lawFor(p);
    hp_.set(p.tuning.detectorHpHz);
    for (int i = 0; i < n; ++i) {
      // Detector: the 20 ms release is the shared smoothing coefficient.
      const float mag = fabsf(hp_.hp(in[i]));
      env_ = mag > env_ ? mag : env_ + smooth::kSmoothCoef * (mag - env_);
      stepGain(dbFast(env_ + kFloor), law);

      // Bypass fade: settled off multiplies by exactly 1.0.
      onMix_ = smooth::step(onMix_, law.onTarget, smooth::kSmoothCoef);
      out[i] = in[i] * dbToGainFast(gainDb_ * onMix_);
    }
  }

  // Applied gain in dB, 0 while off.
  float gainDb() const { return gainDb_ * onMix_; }

 private:
  static constexpr float kFloor = 1e-9f;
  static constexpr float kKneeMinDb = 0.1f;

  // The gain computer's settings for one block.
  struct Law {
    float range, knee, openAt, closeAt, aAtt, aRel, onTarget;
    int holdSmp;
  };

  static Law lawFor(const GateParams& p) {
    const GateTuning& t = p.tuning;
    Law law;
    law.range = t.rangeDb < 0.0f ? t.rangeDb : 0.0f;
    law.knee = t.kneeDb > kKneeMinDb ? t.kneeDb : kKneeMinDb;
    law.openAt = p.thresholdDb;
    law.closeAt = p.thresholdDb - (t.hysteresisDb > 0.0f ? t.hysteresisDb : 0.0f);
    law.aAtt = smooth::coef(t.attackMs * 1e-3f);
    law.aRel = smooth::coef(t.releaseMs * 1e-3f);
    law.onTarget = p.on ? 1.0f : 0.0f;
    law.holdSmp = static_cast<int>((t.holdMs > 0.0f ? t.holdMs : 0.0f) * kSampleRate / 1000.0f);
    return law;
  }

  // Gain computer with hysteresis and a soft knee, then attack, hold and release on the gain in dB.
  void stepGain(float levelDb, const Law& law) {
    if (levelDb >= law.openAt) open_ = true;
    else if (levelDb < law.closeAt) open_ = false;
    float target = 0.0f;
    if (!open_) {
      const float below = law.openAt - levelDb;
      target = below >= law.knee ? law.range : law.range * below / law.knee;
    }
    if (target >= gainDb_) {
      holdLeft_ = law.holdSmp;
      gainDb_ = smooth::step(gainDb_, target, law.aAtt);
    } else if (holdLeft_ > 0) {
      --holdLeft_;
    } else {
      gainDb_ = smooth::step(gainDb_, target, law.aRel);
    }
  }

  OnePole hp_;  // detector side-chain high-pass
  float env_ = 0.0f;
  float gainDb_ = 0.0f;
  float onMix_ = 0.0f;
  int holdLeft_ = 0;
  bool open_ = false;
};

}  // namespace cv
