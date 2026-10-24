#pragma once

#include <cmath>

#include "engine/common.h"

// Building blocks the engines share: constants, a clamp, the one-pole and RBJ biquad filters, and
// the Catmull-Rom read every fractional delay uses.

namespace cv {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kHalfPi = 1.57079632679489661923f;

// Filter corners clamp to this band, so a stray tuning value cannot put a pole on the unit circle.
constexpr float kFilterMinHz = 20.0f;
constexpr float kFilterMaxHz = 0.45f * kSampleRate;
constexpr float kButterworthQ = 0.70710678f;

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Coefficient of a one-pole low-pass with its corner at hz.
inline float onePoleCoef(float hz) {
  return 1.0f - expf(-kTwoPi * clampf(hz, kFilterMinHz, kFilterMaxHz) / kSampleRate);
}

// One-pole low-pass; hp is the input minus the low-pass.
struct OnePole {
  float a = 0.0f;
  float y = 0.0f;

  void set(float hz) { a = onePoleCoef(hz); }

  CV_INLINE float lp(float x) {
    y += a * (x - y);
    return y;
  }

  CV_INLINE float hp(float x) { return x - lp(x); }

  void clear() { y = 0.0f; }
};

// RBJ cookbook biquad in transposed direct form II. Shelves use slope S = 1.
struct Biquad {
  float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
  float z1 = 0.0f, z2 = 0.0f;

  void setLowPass(float hz, float q) {
    const float w0 = kTwoPi * clampf(hz, kFilterMinHz, kFilterMaxHz) / kSampleRate;
    const float cw = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * q);
    const float a0 = 1.0f + alpha;
    b0 = 0.5f * (1.0f - cw) / a0;
    b1 = (1.0f - cw) / a0;
    b2 = b0;
    a1 = -2.0f * cw / a0;
    a2 = (1.0f - alpha) / a0;
  }

  void setHighPass(float hz, float q) {
    const float w0 = kTwoPi * clampf(hz, kFilterMinHz, kFilterMaxHz) / kSampleRate;
    const float cw = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * q);
    const float a0 = 1.0f + alpha;
    b0 = 0.5f * (1.0f + cw) / a0;
    b1 = -(1.0f + cw) / a0;
    b2 = b0;
    a1 = -2.0f * cw / a0;
    a2 = (1.0f - alpha) / a0;
  }

  void setPeak(float hz, float db, float q) {
    const float w0 = kTwoPi * clampf(hz, kFilterMinHz, kFilterMaxHz) / kSampleRate;
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

  void setLowShelf(float hz, float db) { setShelf(hz, db, -1.0f); }
  void setHighShelf(float hz, float db) { setShelf(hz, db, 1.0f); }

  // Takes the coefficients of `o` and keeps this section's state.
  void setCoefsFrom(const Biquad& o) {
    b0 = o.b0;
    b1 = o.b1;
    b2 = o.b2;
    a1 = o.a1;
    a2 = o.a2;
  }

  CV_INLINE float run(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  void clear() { z1 = z2 = 0.0f; }

 private:
  // sign -1 is the low shelf, +1 the high shelf.
  void setShelf(float hz, float db, float sign) {
    const float w0 = kTwoPi * clampf(hz, kFilterMinHz, kFilterMaxHz) / kSampleRate;
    const float amp = powf(10.0f, db / 40.0f);
    const float cw = cosf(w0);
    const float beta = 2.0f * sqrtf(amp) * (sinf(w0) * kButterworthQ);
    const float ap = amp + 1.0f, am = amp - 1.0f;
    const float a0 = ap - sign * am * cw + beta;
    b0 = amp * (ap + sign * am * cw + beta) / a0;
    b1 = -sign * 2.0f * amp * (am + sign * ap * cw) / a0;
    b2 = amp * (ap + sign * am * cw - beta) / a0;
    a1 = sign * 2.0f * (am - sign * ap * cw) / a0;
    a2 = (ap - sign * am * cw - beta) / a0;
  }
};

// Catmull-Rom through xm, x0, x1, x2, at t in [0, 1) past x0.
CV_INLINE float catmullRom(float xm, float x0, float x1, float x2, float t) {
  const float c1 = 0.5f * (x1 - xm);
  const float c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
  const float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
  return x0 + t * (c1 + t * (c2 + t * c3));
}

// The same curve as tap weights on xm, x0, x1, x2, for a read whose fraction t stays fixed.
struct CatmullRomWeights {
  float m, z, p, q;
};

inline CatmullRomWeights catmullRomWeights(float t) {
  const float t2 = t * t;
  const float t3 = t2 * t;
  return {-0.5f * t + t2 - 0.5f * t3, 1.0f - 2.5f * t2 + 1.5f * t3, 0.5f * t + 2.0f * t2 - 1.5f * t3,
          -0.5f * t2 + 0.5f * t3};
}

// Wraps a ring index that sits within one length of [0, Len). Delay reads stay that close to the
// write head, so one step is enough; the final bound is never taken and lets the compiler prove
// every index in range.
template <int Len>
CV_INLINE int wrapOnce(int i) {
  if (i < 0) i += Len;
  else if (i >= Len) i -= Len;
  return static_cast<unsigned>(i) < static_cast<unsigned>(Len) ? i : 0;
}

// Catmull-Rom read `delay` samples behind `head` in a ring of Len samples. The caller keeps the
// delay inside [2, Len - 3], so the four taps never cross the head.
template <int Len>
CV_INLINE float readRing(const float* ring, int head, float delay) {
  const float pos = static_cast<float>(head) - delay;
  const float fl = floorf(pos);
  const int i0 = wrapOnce<Len>(static_cast<int>(fl));
  return catmullRom(ring[wrapOnce<Len>(i0 - 1)], ring[i0], ring[wrapOnce<Len>(i0 + 1)], ring[wrapOnce<Len>(i0 + 2)],
                    pos - fl);
}

}  // namespace cv
