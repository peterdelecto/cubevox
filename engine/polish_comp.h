#pragma once

#include <array>
#include <cmath>
#include <cstring>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Polish compressor: a feed-forward vocal leveller at the end of the chain. A 4-pole high-passed
// sidechain feeds an RMS detector, a soft-knee gain computer works in dB, and the gain reduction
// is smoothed with separate attack and release before it becomes a linear gain. Settled off is a
// bit-exact copy.

namespace cv {

// The tuning is FIRM (owner 2026-10-24): -30 dB threshold, 4:1 ratio, 4 dB knee, 3 ms attack,
// 100 ms release, 18 dB cap. Its makeup is solved by level_test on the clips and the -23 dBFS program.
struct PolishCompTuning {
  float rmsMs = 10.0f;          // detector time constant (mean-square one-pole)
  float sidechainHz = 150.0f;   // 4-pole high-pass corner ahead of the detector
  float thresholdDb = -30.0f;   // dBFS RMS
  float ratio = 4.0f;
  float kneeDb = 4.0f;          // soft knee width, centred on the threshold
  float attackMs = 3.0f;        // gain reduction smoothing, rising
  float releaseMs = 100.0f;     // gain reduction smoothing, falling
  float maxReductionDb = 18.0f;
  float trimDb = 5.85f;         // makeup, active path only; level rule on program material
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
      std::memmove(out, in, sizeof(float) * static_cast<size_t>(n));
      return;
    }
    dirty_ = true;
    if (!coefsValid_ || !sameTuning(p.tuning, applied_)) applyTuning(p.tuning);

    for (int i = 0; i < n; i += kBlock) chunk(in + i, out + i, n - i < kBlock ? n - i : kBlock, target);
  }

 private:
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

  // The sidechain runs two 2-pole Butterworth high-passes in series, which puts 50 Hz 38 dB down for
  // a 150 Hz corner and the corner itself 6 dB down.
  void applyTuning(const PolishCompTuning& t) {
    hp1_.setHighPass(t.sidechainHz, kButterworthQ);
    hp2_.setCoefsFrom(hp1_);
    c_ = coefs(t);
    applied_ = t;
    coefsValid_ = true;
  }

  static Coefs coefs(const PolishCompTuning& t) {
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

  // Pass 1 over one chunk: the dB gain of each sample, makeup included. The filter states, the
  // detector and the smoothed reduction stay in locals for the whole chunk.
  void gainsDb(const float* in, float* db, int n) {
    const Coefs c = c_;
    Biquad h1 = hp1_;
    Biquad h2 = hp2_;
    float ms = ms_;
    float gr = grDb_;
    for (int i = 0; i < n; ++i) {
      const float side = h2.run(h1.run(in[i] + kSidechainGuard) + kSidechainGuard);
      ms += c.aDet * (side * side + kPowerFloor - ms);
      const float want = reductionDb((kDbPerLog2 / 2) * log2Fast(ms), c);  // mean-square power to dB
      gr = smooth::step(gr, want, want > gr ? c.aAtk : c.aRel);
      db[i] = c.trimDb - gr;
    }
    hp1_ = h1;
    hp2_ = h2;
    ms_ = ms;
    grDb_ = gr;
  }

  // Pass 2 over one chunk: turns each dB gain linear and mixes it in by the on/off smoother. A
  // smoother already on its target is not stepped.
  void chunk(const float* in, float* out, int n, float target) {
    std::array<float, kBlock> db;
    gainsDb(in, db.data(), n);
    float active = active_;
    if (active == target) {
#pragma GCC unroll 2
      for (int i = 0; i < n; ++i) {
        out[i] = in[i] * (1.0f + active * (dbToGainFast(db[static_cast<size_t>(i)]) - 1.0f));
      }
      return;
    }
    for (int i = 0; i < n; ++i) {
      active = smooth::step(active, target, smooth::kSmoothCoef);
      out[i] = in[i] * (1.0f + active * (dbToGainFast(db[static_cast<size_t>(i)]) - 1.0f));
    }
    active_ = active;
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
