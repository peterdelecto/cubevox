#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Gate: a peak detector drives a gain computer with hysteresis, hold, and attack/release
// smoothing of the gain in dB. The panel GATE keeps the default tuning: a detector behind a
// one-pole high-pass and a soft knee. The slot-1 Input gate runs inputGateTuning(): a plain peak
// detector, a hard range and times given as travel times.
// The per-sample dB conversions run through fastmath. fastmath_test pins both to
// libm, and gate_test checks the applied gain against it.

namespace cv {

struct GateTuning {
  float attackMs = 1.0f;
  float holdMs = 150.0f;
  float releaseMs = 20.0f;
  float rangeDb = -40.0f;       // attenuation when closed
  float kneeDb = 12.0f;         // soft knee width below the threshold; 0 is a hard range
  float detectorHpHz = 120.0f;  // detector side-chain highpass; 0 leaves the detector unfiltered
  float hysteresisDb = 6.0f;    // close level = open threshold - hysteresis
  float detectorAttackMs = 0.0f;    // detector rise time constant; 0 follows a rise at once
  float detectorReleaseMs = 20.0f;  // detector fall time constant

  // With travelTimes false, attackMs and releaseMs are time constants of the one-pole on the gain
  // in dB. With it true, attackMs is the time the gain takes to rise from the closed range to
  // within 1 dB of open, and releaseMs the time it takes to fall three quarters of the way to the
  // closed range (60 dB at -80 dB), so each value names how long its move takes.
  bool travelTimes = false;
};

// The Input gate's pots. THRESHOLD runs linear in dB, DECAY (the release time) log.
constexpr float kInputGateThresholdMinDb = -80.0f;
constexpr float kInputGateThresholdMaxDb = -30.0f;
constexpr float kInputGateDecayMinMs = 20.0f;
constexpr float kInputGateDecayMaxMs = 2000.0f;

// The Input gate's tuning before its DECAY pot sets the release (spec 2026-10-30-input-conditioning).
inline GateTuning inputGateTuning() {
  GateTuning t;
  t.attackMs = 0.5f;
  t.holdMs = 50.0f;
  t.releaseMs = 200.0f;
  t.rangeDb = -80.0f;
  t.kneeDb = 0.0f;
  t.detectorHpHz = 0.0f;
  t.hysteresisDb = 3.0f;
  t.detectorAttackMs = 0.1f;
  t.detectorReleaseMs = 5.0f;
  t.travelTimes = true;
  return t;
}

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
    peak_ = 0.0f;
    gainDb_ = 0.0f;
    onMix_ = 0.0f;
    holdLeft_ = 0;
    open_ = false;
  }

  void process(const float* in, float* out, int n, const GateParams& p) {
    const Law law = lawFor(p);
    hp_.set(p.tuning.detectorHpHz);
    for (int i = 0; i < n; ++i) {
      // Detector: a peak follower on the rectified signal.
      const float mag = fabsf(law.detectorHp ? hp_.hp(in[i]) : in[i]);
      if (mag <= env_) env_ += law.aDetRel * (mag - env_);
      else if (law.aDetAtt >= 1.0f) env_ = mag;
      else env_ += law.aDetAtt * (mag - env_);
      if (env_ > peak_) peak_ = env_;
      stepGain(dbFast(env_ + kFloor), law);

      // Bypass fade: settled off multiplies by exactly 1.0.
      onMix_ = smooth::step(onMix_, law.onTarget, smooth::kSmoothCoef);
      out[i] = in[i] * dbToGainFast(gainDb_ * onMix_);
    }
  }

  // Applied gain in dB, 0 while off.
  float gainDb() const { return gainDb_ * onMix_; }

  // The highest detector level (linear, full scale 1) since the previous call, and the window
  // restarts. No division, so the audio interrupt can call it.
  float takeDetectorPeak() {
    const float peak = peak_;
    peak_ = 0.0f;
    return peak;
  }

 private:
  static constexpr float kFloor = 1e-9f;
  static constexpr float kKneeMinDb = 0.1f;

  // Travel times. A one-pole covers a fraction f of a step in -ln(1 - f) time constants. The rise
  // ends kAttackSettleDb short of open, so its divisor is ln(range / kAttackSettleDb). The fall
  // covers 3/4 of the range, so its divisor is ln 4.
  static constexpr float kAttackSettleDb = 1.0f;
  static constexpr float kReleaseTravelTimeConstants = 1.38629436f;

  // The gain computer's settings for one block.
  struct Law {
    float range, knee, openAt, closeAt, aAtt, aRel, aDetAtt, aDetRel, onTarget;
    int holdSmp;
    bool detectorHp;
  };

  static Law lawFor(const GateParams& p) {
    const GateTuning& t = p.tuning;
    Law law;
    law.range = t.rangeDb < 0.0f ? t.rangeDb : 0.0f;
    law.knee = t.kneeDb <= 0.0f ? 0.0f : (t.kneeDb > kKneeMinDb ? t.kneeDb : kKneeMinDb);
    law.openAt = p.thresholdDb;
    law.closeAt = p.thresholdDb - (t.hysteresisDb > 0.0f ? t.hysteresisDb : 0.0f);
    float attackSec = t.attackMs * 1e-3f;
    float releaseSec = t.releaseMs * 1e-3f;
    if (t.travelTimes) {
      const float riseCovered = -law.range / kAttackSettleDb;  // closed range over the settle margin
      if (riseCovered > 1.0f) attackSec /= logf(riseCovered);
      releaseSec /= kReleaseTravelTimeConstants;
    }
    law.aAtt = smooth::coef(attackSec);
    law.aRel = smooth::coef(releaseSec);
    law.aDetAtt = smooth::coef(t.detectorAttackMs / 1000.0f);
    law.aDetRel = smooth::coef(t.detectorReleaseMs / 1000.0f);
    law.detectorHp = t.detectorHpHz > 0.0f;
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
      target = below >= law.knee ? law.range : law.range * below / law.knee;  // knee 0: always the range
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
  float peak_ = 0.0f;  // highest env_ since takeDetectorPeak()
  float gainDb_ = 0.0f;
  float onMix_ = 0.0f;
  int holdLeft_ = 0;
  bool open_ = false;
};

}  // namespace cv
