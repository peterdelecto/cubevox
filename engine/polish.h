#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/smooth.h"

// Final EQ: 2-pole Butterworth high-pass, low-mid dip, presence peak, air shelf.
// RBJ biquads, TDF-II, coefficients rebuilt per block. Settled off is a bit-exact copy.

namespace cv {

struct PolishTuning {
  float dipQ = 1.0f;
  float presenceQ = 0.7f;
  float airHz = 10000.0f;
};

struct PolishParams {
  bool on = true;
  float hpHz = 90.0f;          // 40..200
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

    const PolishTuning& t = p.tuning;
    hp_.setHighPass(p.hpHz);
    dip_.setPeak(p.dipHz, p.dipDb, t.dipQ);
    presence_.setPeak(p.presenceHz, p.presenceDb, t.presenceQ);
    air_.setHighShelf(t.airHz, p.airDb);

    const float a = smooth::coef(smooth::kSmoothSec);
    for (int i = 0; i < n; ++i) {
      active_ = smooth::step(active_, target, a);
      float x = hp_.run(in[i]);
      x = dip_.run(x);
      x = presence_.run(x);
      x = air_.run(x);
      out[i] = in[i] + active_ * (x - in[i]);
    }
  }

 private:
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr float kMinHz = 20.0f;
  static constexpr float kMaxHz = 0.45f * kSampleRate;
  static constexpr float kButterworthQ = 0.70710678f;

  static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

  // RBJ biquad, transposed direct form II.
  struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void setHighPass(float hz) {
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

    void setPeak(float hz, float db, float q) {
      const float w0 = kTwoPi * clampf(hz, kMinHz, kMaxHz) / kSampleRate;
      const float amp = powf(10.0f, db / 40.0f);
      const float cw = cosf(w0);
      const float alpha = sinf(w0) / (2.0f * (q < 0.1f ? 0.1f : q));
      const float a0 = 1.0f + alpha / amp;
      b0 = (1.0f + alpha * amp) / a0;
      b1 = -2.0f * cw / a0;
      b2 = (1.0f - alpha * amp) / a0;
      a1 = b1;
      a2 = (1.0f - alpha / amp) / a0;
    }

    // Shelf slope S = 1.
    void setHighShelf(float hz, float db) {
      const float w0 = kTwoPi * clampf(hz, kMinHz, kMaxHz) / kSampleRate;
      const float amp = powf(10.0f, db / 40.0f);
      const float cw = cosf(w0);
      const float beta = 2.0f * sqrtf(amp) * (sinf(w0) * kButterworthQ);
      const float ap = amp + 1.0f, am = amp - 1.0f;
      const float a0 = ap - am * cw + beta;
      b0 = amp * (ap + am * cw + beta) / a0;
      b1 = -2.0f * amp * (am + ap * cw) / a0;
      b2 = amp * (ap + am * cw - beta) / a0;
      a1 = 2.0f * (am - ap * cw) / a0;
      a2 = (ap - am * cw - beta) / a0;
    }

    float run(float x) {
      const float y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }

    void clear() { z1 = z2 = 0.0f; }
  };

  void clearChain() {
    for (Biquad* b : {&hp_, &dip_, &presence_, &air_}) b->clear();
    dirty_ = false;
  }

  Biquad hp_, dip_, presence_, air_;
  float active_ = 0.0f;
  bool dirty_ = false;
};

}  // namespace cv
