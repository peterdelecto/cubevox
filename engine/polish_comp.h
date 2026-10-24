#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Polish compressor: a gentle feed-forward vocal leveller at the end of the chain. A high-passed
// sidechain feeds an RMS detector, a soft-knee gain computer works in dB, and the gain reduction
// is smoothed with separate attack and release before it becomes a linear gain. Settled off is a
// bit-exact copy.

namespace cv {

struct PolishCompTuning {
  float rmsMs = 10.0f;          // detector time constant (mean-square one-pole)
  float sidechainHz = 150.0f;   // 2-pole high-pass ahead of the detector
  float thresholdDb = -24.0f;   // dBFS RMS, a little under the voice loops' average level
  float ratio = 2.2f;
  float kneeDb = 6.0f;          // soft knee width, centred on the threshold
  float attackMs = 10.0f;       // gain reduction smoothing, rising
  float releaseMs = 150.0f;     // gain reduction smoothing, falling
  float maxReductionDb = 12.0f;
  float trimDb = 1.9f;          // makeup, active path only; level rule on program material
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

    const Coefs c = coefs(p.tuning);
    for (int i = 0; i < n; ++i) {
      active_ = smooth::step(active_, target, smooth::kSmoothCoef);
      const float gain = nextGain(in[i], c);
      out[i] = in[i] * (1.0f + active_ * (gain - 1.0f));
    }
  }

 private:
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr float kMinHz = 20.0f;
  static constexpr float kMaxHz = 0.45f * kSampleRate;
  static constexpr float kButterworthQ = 0.70710678f;
  static constexpr float kDbPerLog2 = 3.01029995664f;  // 10 log10(2): mean-square to dB
  static constexpr float kLog2PerDb = 0.16609640474f;  // log2(10) / 20: dB to amplitude

  // Added to the sidechain input and to the squared sidechain. They hold the filter states and
  // the detector above the denormal range when the input is digital silence, and floor the
  // detector at -200 dBFS so log2Fast never sees zero.
  static constexpr float kSidechainGuard = 1e-10f;
  static constexpr float kPowerFloor = 1e-20f;

  static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

  // 2-pole Butterworth high-pass, RBJ form, transposed direct form II.
  struct HighPass {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void set(float hz) {
      const float w0 = kTwoPi * clampf(hz, kMinHz, kMaxHz) / kSampleRate;
      const float cw = cosf(w0);
      const float alpha = sinf(w0) / (2.0f * kButterworthQ);
      const float a0 = 1.0f + alpha;
      b0 = (1.0f + cw) * 0.5f / a0;
      b1 = -(1.0f + cw) / a0;
      b2 = b0;
      a1 = -2.0f * cw / a0;
      a2 = (1.0f - alpha) / a0;
    }

    float run(float x) {
      const float y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }

    void clear() { z1 = z2 = 0.0f; }
  };

  // Everything process() derives from the tuning once per block.
  struct Coefs {
    float aDet, aAtk, aRel;
    float thresholdDb, halfKneeDb, slope, kneeScale, maxDb, trimDb;
  };

  Coefs coefs(const PolishCompTuning& t) {
    hp_.set(t.sidechainHz);
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
    const float side = hp_.run(x + kSidechainGuard);
    ms_ += c.aDet * (side * side + kPowerFloor - ms_);
    const float want = reductionDb(kDbPerLog2 * log2Fast(ms_), c);
    grDb_ = smooth::step(grDb_, want, want > grDb_ ? c.aAtk : c.aRel);
    return exp2Fast((c.trimDb - grDb_) * kLog2PerDb);
  }

  void clearState() {
    hp_.clear();
    ms_ = kPowerFloor;
    grDb_ = 0.0f;
    dirty_ = false;
  }

  HighPass hp_;
  float ms_ = kPowerFloor;
  float grDb_ = 0.0f;
  float active_ = 0.0f;
  bool dirty_ = false;
};

}  // namespace cv
