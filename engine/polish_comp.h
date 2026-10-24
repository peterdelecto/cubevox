#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Polish compressor: a feed-forward vocal leveller at the end of the chain. A 4-pole high-passed
// sidechain feeds an RMS detector, a soft-knee gain computer works in dB, and the gain reduction
// is smoothed with separate attack and release before it becomes a linear gain. Settled off is a
// bit-exact copy.

namespace cv {

// Three named tunings. FIRM is the default (owner 2026-10-24). The detector window and the sidechain
// filter are the same in all three, and each has its own makeup, solved by level_test on the
// -23 dBFS program.
//
//           threshold  ratio  knee  attack  release  max
//   GENTLE  -24 dB     2.2:1  6 dB  10 ms   150 ms   12 dB
//   MEDIUM  -28 dB     3:1    6 dB   5 ms   120 ms   15 dB
//   FIRM    -30 dB     4:1    4 dB   3 ms   100 ms   18 dB
struct PolishCompTuning {
  float rmsMs = 10.0f;          // detector time constant (mean-square one-pole)
  float sidechainHz = 150.0f;   // 4-pole high-pass corner ahead of the detector
  float thresholdDb = -30.0f;   // dBFS RMS
  float ratio = 4.0f;
  float kneeDb = 4.0f;          // soft knee width, centred on the threshold
  float attackMs = 3.0f;        // gain reduction smoothing, rising
  float releaseMs = 100.0f;     // gain reduction smoothing, falling
  float maxReductionDb = 18.0f;
  float trimDb = 5.9f;          // makeup, active path only; level rule on program material

  static PolishCompTuning gentle() {
    PolishCompTuning t;
    t.thresholdDb = -24.0f;
    t.ratio = 2.2f;
    t.kneeDb = 6.0f;
    t.attackMs = 10.0f;
    t.releaseMs = 150.0f;
    t.maxReductionDb = 12.0f;
    t.trimDb = 1.5f;
    return t;
  }

  static PolishCompTuning medium() {
    PolishCompTuning t;
    t.thresholdDb = -28.0f;
    t.ratio = 3.0f;
    t.kneeDb = 6.0f;
    t.attackMs = 5.0f;
    t.releaseMs = 120.0f;
    t.maxReductionDb = 15.0f;
    t.trimDb = 4.1f;
    return t;
  }

  static PolishCompTuning firm() { return PolishCompTuning(); }
};

struct PolishCompParams {
  bool on = true;
  PolishCompTuning tuning;
};

class PolishComp {
 public:
  PolishComp() { reset(); }

  void reset() {
    clearState();
    active_ = 0.0f;
  }

  // Gain reduction the stage applies at the last sample, in dB. 0 while settled off.
  float gainReductionDb() const { return grDb_ * active_; }

  // `out` may alias `in`.
  void process(const float* in, float* out, int n, const PolishCompParams& p) {
    const float target = p.on ? 1.0f : 0.0f;

    if (active_ == 0.0f && target == 0.0f) {
      if (dirty_) clearState();
      for (int i = 0; i < n; ++i) out[i] = in[i];
      return;
    }
    dirty_ = true;
    if (!coefsValid_ || !sameTuning(p.tuning, applied_)) applyTuning(p.tuning);

    for (int i = 0; i < n; ++i) {
      active_ = smooth::step(active_, target, smooth::kSmoothCoef);
      const float gain = nextGain(in[i], c_);
      out[i] = in[i] * (1.0f + active_ * (gain - 1.0f));
    }
  }

 private:
  static constexpr float kPowerDbPerLog2 = 3.01029995664f;  // 10 log10(2): mean-square to dB

  // Added to the input of each high-pass section and to the squared sidechain. They hold the
  // filter states and the detector above the denormal range when the input is digital silence, and
  // floor the detector at -200 dBFS so log2Fast never sees zero.
  static constexpr float kSidechainGuard = 1e-10f;
  static constexpr float kPowerFloor = 1e-20f;

  // Everything process() derives from the tuning, rebuilt only when the tuning changes.
  struct Coefs {
    float aDet, aAtk, aRel;
    float thresholdDb, halfKneeDb, slope, kneeScale, maxDb, trimDb;
  };

  static bool sameTuning(const PolishCompTuning& a, const PolishCompTuning& b) {
    return a.rmsMs == b.rmsMs && a.sidechainHz == b.sidechainHz && a.thresholdDb == b.thresholdDb &&
           a.ratio == b.ratio && a.kneeDb == b.kneeDb && a.attackMs == b.attackMs &&
           a.releaseMs == b.releaseMs && a.maxReductionDb == b.maxReductionDb && a.trimDb == b.trimDb;
  }

  void applyTuning(const PolishCompTuning& t) {
    c_ = coefs(t);
    applied_ = t;
    coefsValid_ = true;
  }

  // The sidechain runs two 2-pole Butterworth high-passes in series, which puts 50 Hz 38 dB down for
  // a 150 Hz corner and the corner itself 6 dB down.
  Coefs coefs(const PolishCompTuning& t) {
    hp1_.setHighPass(t.sidechainHz, kButterworthQ);
    hp2_.setCoefsFrom(hp1_);
    const float knee = t.kneeDb > 0.0f ? t.kneeDb : 0.0f;
    const float ratio = t.ratio > 1.0f ? t.ratio : 1.0f;
    Coefs c;
    c.aDet = smooth::coef(t.rmsMs * 1e-3f);
    c.aAtk = smooth::coef(t.attackMs * 1e-3f);
    c.aRel = smooth::coef(t.releaseMs * 1e-3f);
    c.thresholdDb = t.thresholdDb;
    c.halfKneeDb = 0.5f * knee;
    c.slope = 1.0f - 1.0f / ratio;
    c.kneeScale = knee > 0.0f ? c.slope / (2.0f * knee) : 0.0f;
    c.maxDb = t.maxReductionDb;
    c.trimDb = t.trimDb;
    return c;
  }

  // Gain reduction in dB for a detector level: none below the knee, quadratic through it, then
  // (1 - 1/ratio) per dB over the threshold, capped at maxDb.
  static float reductionDb(float levelDb, const Coefs& c) {
    const float over = levelDb - c.thresholdDb;
    if (over <= -c.halfKneeDb) return 0.0f;
    const float below = over + c.halfKneeDb;
    const float gr = over >= c.halfKneeDb ? c.slope * over : c.kneeScale * below * below;
    return gr < c.maxDb ? gr : c.maxDb;
  }

  // Linear gain for one input sample, makeup included.
  float nextGain(float x, const Coefs& c) {
    const float side = hp2_.run(hp1_.run(x + kSidechainGuard) + kSidechainGuard);
    ms_ += c.aDet * (side * side + kPowerFloor - ms_);
    const float want = reductionDb(kPowerDbPerLog2 * log2Fast(ms_), c);
    grDb_ = smooth::step(grDb_, want, want > grDb_ ? c.aAtk : c.aRel);
    return dbToGainFast(c.trimDb - grDb_);
  }

  void clearState() {
    hp1_.clear();
    hp2_.clear();
    ms_ = kPowerFloor;
    grDb_ = 0.0f;
    dirty_ = false;
  }

  Biquad hp1_;
  Biquad hp2_;
  float ms_ = kPowerFloor;
  float grDb_ = 0.0f;
  float active_ = 0.0f;
  bool dirty_ = false;
  Coefs c_{};
  PolishCompTuning applied_;  // the tuning c_ was built from
  bool coefsValid_ = false;
};

}  // namespace cv
