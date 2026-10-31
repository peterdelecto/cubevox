#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/deesser.h"
#include "engine/dsp.h"
#include "engine/smooth.h"

// POLISH EQ voiced for Adam's voice, one stage in two places under one ON/OFF. The front half runs
// ahead of slot 1 and holds a 70 Hz high-pass, a 6 kHz low-pass and a de-esser. The back half runs
// after POLISH COMP and holds a 300 Hz dip, a 3.5 kHz presence peak and the trim.
//
// Each half crossfades with its input through its own 20 ms smoother on the one `on`, so a click
// fades both together. Settled off, each half is a bit-exact copy and holds no filter state.
// RBJ biquads, TDF-II. The back half rebuilds its coefficients only when a setting changes.
// Spec docs/specs/2026-10-31-polish-eq-adam-voice-design.md.

namespace cv {

// Front cutoffs. Both filters are 4th-order Butterworth, two biquads each.
constexpr float kPolishHighPassHz = 70.0f;
constexpr float kPolishLowPassHz = 6000.0f;

// Section Qs of a 4th-order Butterworth, 1 / (2 cos(pi / 8)) and 1 / (2 cos(3 pi / 8)).
constexpr float kButterworth4Q1 = 0.54119610f;
constexpr float kButterworth4Q2 = 1.30656296f;

struct PolishTuning {
  float dipQ = 1.0f;
  float presenceQ = 0.7f;
  float trimDb = 1.3f;  // active back half only; level rule on program material
  DeEsserTuning deEsser;
};

struct PolishParams {
  bool on = true;
  float dipHz = 300.0f;        // 150..600
  float dipDb = -2.5f;         // -6..0
  float presenceHz = 3500.0f;  // 2000..6000
  float presenceDb = 1.5f;     // 0..6
  PolishTuning tuning;
};

class Polish {
 public:
  Polish() {
    for (int i = 0; i < 2; ++i) {
      const float q = i == 0 ? kButterworth4Q1 : kButterworth4Q2;
      hp_[static_cast<size_t>(i)].setHighPass(kPolishHighPassHz, q);
      lp_[static_cast<size_t>(i)].setLowPass(kPolishLowPassHz, q);
    }
    reset();
  }

  void reset() {
    clearFront();
    clearBack();
    frontActive_ = 0.0f;
    backActive_ = 0.0f;
  }

  // Front half. `in` and `out` may be the same buffer. Takes any `n`.
  void processFront(const float* in, float* out, int n, const PolishParams& p) {
    const float target = p.on ? 1.0f : 0.0f;
    if (frontActive_ == 0.0f && target == 0.0f) {
      if (frontDirty_) clearFront();
      copy(in, out, n);
      return;
    }
    frontDirty_ = true;
    for (int i = 0; i < n; i += kBlock) {
      frontChunk(in + i, out + i, n - i < kBlock ? n - i : kBlock, target, p.tuning.deEsser);
    }
  }

  // Back half. `in` and `out` may be the same buffer. Takes any `n`.
  void processBack(const float* in, float* out, int n, const PolishParams& p) {
    const float target = p.on ? 1.0f : 0.0f;
    if (backActive_ == 0.0f && target == 0.0f) {
      if (backDirty_) clearBack();
      copy(in, out, n);
      return;
    }
    backDirty_ = true;
    if (!shapeValid_ || !sameShape(p, applied_)) applyShape(p);

    for (int i = 0; i < n; ++i) {
      backActive_ = smooth::step(backActive_, target, smooth::kSmoothCoef);
      const float x = presence_.run(dip_.run(in[i])) * trim_;
      out[i] = in[i] + backActive_ * (x - in[i]);
    }
  }

  // Gain reduction the de-esser holds now.
  float deEsserCutDb() const { return deEsser_.cutDb(); }

 private:
  // The settings the back half's coefficients come from.
  struct BackShape {
    float dipHz, dipDb, presenceHz, presenceDb, dipQ, presenceQ, trimDb;
  };

  static BackShape shapeOf(const PolishParams& p) {
    return {p.dipHz, p.dipDb, p.presenceHz, p.presenceDb, p.tuning.dipQ, p.tuning.presenceQ, p.tuning.trimDb};
  }

  static bool sameShape(const PolishParams& p, const BackShape& s) {
    const BackShape a = shapeOf(p);
    return a.dipHz == s.dipHz && a.dipDb == s.dipDb && a.presenceHz == s.presenceHz &&
           a.presenceDb == s.presenceDb && a.dipQ == s.dipQ && a.presenceQ == s.presenceQ &&
           a.trimDb == s.trimDb;
  }

  static void copy(const float* in, float* out, int n) {
    if (out == in) return;
    for (int i = 0; i < n; ++i) out[i] = in[i];
  }

  // Filters and de-esses up to kBlock samples, then mixes them with the input by the smoother.
  void frontChunk(const float* in, float* out, int n, float target, const DeEsserTuning& t) {
    std::array<float, kBlock> y;
    for (int i = 0; i < n; ++i) {
      y[static_cast<size_t>(i)] = lp_[1].run(lp_[0].run(hp_[1].run(hp_[0].run(in[i]))));
    }
    deEsser_.process(y.data(), n, t);
    for (int i = 0; i < n; ++i) {
      frontActive_ = smooth::step(frontActive_, target, smooth::kSmoothCoef);
      out[i] = in[i] + frontActive_ * (y[static_cast<size_t>(i)] - in[i]);
    }
  }

  // Rebuilds the back coefficients and keeps each section's state.
  void applyShape(const PolishParams& p) {
    const PolishTuning& t = p.tuning;
    dip_.setPeak(p.dipHz, p.dipDb, t.dipQ);
    presence_.setPeak(p.presenceHz, p.presenceDb, t.presenceQ);
    trim_ = powf(10.0f, t.trimDb / 20.0f);
    applied_ = shapeOf(p);
    shapeValid_ = true;
  }

  void clearFront() {
    for (Biquad& b : hp_) b.clear();
    for (Biquad& b : lp_) b.clear();
    deEsser_.reset();
    frontDirty_ = false;
  }

  void clearBack() {
    dip_.clear();
    presence_.clear();
    backDirty_ = false;
  }

  std::array<Biquad, 2> hp_, lp_;
  DeEsser deEsser_;
  Biquad dip_, presence_;
  float trim_ = 1.0f;
  float frontActive_ = 0.0f;
  float backActive_ = 0.0f;
  bool frontDirty_ = false;
  bool backDirty_ = false;
  BackShape applied_{};  // the settings the back coefficients were built from
  bool shapeValid_ = false;
};

}  // namespace cv
