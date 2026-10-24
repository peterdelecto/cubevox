#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/smooth.h"

// Final EQ: 2-pole Butterworth high-pass, low-mid dip, presence peak, air shelf.
// RBJ biquads, TDF-II, coefficients rebuilt only when a setting changes. Settled
// off is a bit-exact copy.

namespace cv {

struct PolishTuning {
  float dipQ = 1.0f;
  float presenceQ = 0.7f;
  float airHz = 10000.0f;
  float trimDb = 1.6f;  // active path only; level rule on program material
};

struct PolishParams {
  bool on = true;
  float hpHz = 110.0f;         // 40..200
  float dipHz = 300.0f;        // 150..600
  float dipDb = -2.5f;         // -6..0
  float presenceHz = 3500.0f;  // 2000..6000
  float presenceDb = 1.5f;     // 0..6
  float airDb = 1.5f;          // 0..4
  PolishTuning tuning;
};

class Polish {
 public:
  Polish() { reset(); }

  void reset() {
    clearChain();
    active_ = 0.0f;
    dirty_ = false;
  }

  void process(const float* in, float* out, int n, const PolishParams& p) {
    const float target = p.on ? 1.0f : 0.0f;

    if (active_ == 0.0f && target == 0.0f) {
      if (dirty_) clearChain();
      for (int i = 0; i < n; ++i) out[i] = in[i];
      return;
    }
    dirty_ = true;
    if (!shapeValid_ || !sameShape(p, applied_)) applyShape(p);

    for (int i = 0; i < n; ++i) {
      active_ = smooth::step(active_, target, smooth::kSmoothCoef);
      const float x = air_.run(presence_.run(dip_.run(hp_.run(in[i])))) * trim_;
      out[i] = in[i] + active_ * (x - in[i]);
    }
  }

 private:
  static bool sameShape(const PolishParams& a, const PolishParams& b) {
    return a.hpHz == b.hpHz && a.dipHz == b.dipHz && a.dipDb == b.dipDb &&
           a.presenceHz == b.presenceHz && a.presenceDb == b.presenceDb && a.airDb == b.airDb &&
           a.tuning.dipQ == b.tuning.dipQ && a.tuning.presenceQ == b.tuning.presenceQ &&
           a.tuning.airHz == b.tuning.airHz && a.tuning.trimDb == b.tuning.trimDb;
  }

  // Rebuilds the coefficients and keeps each section's state.
  void applyShape(const PolishParams& p) {
    const PolishTuning& t = p.tuning;
    hp_.setHighPass(p.hpHz, kButterworthQ);
    dip_.setPeak(p.dipHz, p.dipDb, t.dipQ);
    presence_.setPeak(p.presenceHz, p.presenceDb, t.presenceQ);
    air_.setHighShelf(t.airHz, p.airDb);
    trim_ = powf(10.0f, t.trimDb / 20.0f);
    applied_ = p;
    shapeValid_ = true;
  }

  void clearChain() {
    for (Biquad* b : {&hp_, &dip_, &presence_, &air_}) b->clear();
    dirty_ = false;
  }

  Biquad hp_, dip_, presence_, air_;
  float trim_ = 1.0f;
  float active_ = 0.0f;
  bool dirty_ = false;
  PolishParams applied_;  // the settings the coefficients were built from
  bool shapeValid_ = false;
};

}  // namespace cv
