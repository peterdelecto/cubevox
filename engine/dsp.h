#pragma once

#include <cmath>

#include "engine/common.h"

// Building blocks the engines share: constants, a clamp, the dB-to-gain law and the log taper, the
// one-pole and RBJ biquad filters, and the Catmull-Rom forms. Unison reads its delay line with
// readRing, Echo and the pitch voices call catmullRom or catmullRomWeights on their own taps, and
// Mob's voices and the reverbs' Delay read linearly.

namespace cv {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kHalfPi = 1.57079632679489661923f;

// Filter corners clamp to this band, so a stray tuning value cannot put a pole on the unit circle.
constexpr float kFilterMinHz = 20.0f;
constexpr float kFilterMaxHz = 0.45f * kSampleRate;
constexpr float kButterworthQ = 0.70710678f;

// x limited to [lo, hi]. NaN reads as lo, so a NaN never passes a boundary clamp. Written with
// fmin and fmax, which the M7 compiles to vminnm and vmaxnm with no flag transfer.
constexpr float clampf(float x, float lo, float hi) { return __builtin_fminf(__builtin_fmaxf(x, lo), hi); }

// Zeroes n floats a word at a time, eight to a pass. The board's memset (board/fastmem.cpp) clears at
// the same speed, so this is a plain loop kept for its callers. The empty asm keeps the compiler from
// turning the loops into a memset call.
inline void zeroFloats(float* p, size_t n) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    p[i] = 0.0f;
    p[i + 1] = 0.0f;
    p[i + 2] = 0.0f;
    p[i + 3] = 0.0f;
    p[i + 4] = 0.0f;
    p[i + 5] = 0.0f;
    p[i + 6] = 0.0f;
    p[i + 7] = 0.0f;
    __asm__ volatile("" ::: "memory");
  }
  for (; i < n; ++i) {
    p[i] = 0.0f;
    __asm__ volatile("" ::: "memory");
  }
}

// A knob position p in [0, 1] mapped geometrically from `from` at 0 to `to` at 1. The panel's log
// knobs and the RESPONSE knob run on it.
inline float logTaper(float from, float to, float p) { return from * powf(to / from, p); }

// 10^(db / 20) exactly, through libm: for a value computed once a block or when a setting changes.
// dbToGainFast in fastmath.h is the form for a sample loop.
inline float dbToGain(float db) { return powf(10.0f, db / 20.0f); }

// Coefficient of a one-pole low-pass with its corner at hz, for a filter that runs at rate Hz. The
// corner clamps to 0.45 of that rate.
inline float onePoleCoefAt(float hz, float rate) {
  return 1.0f - expf(-kTwoPi * clampf(hz, kFilterMinHz, 0.45f * rate) / rate);
}

// The same at the engine rate.
inline float onePoleCoef(float hz) { return onePoleCoefAt(hz, static_cast<float>(kSampleRate)); }

// One-pole low-pass; hp is the input minus the low-pass.
struct OnePole {
  float a = 0.0f;
  float y = 0.0f;

  void set(float hz) { a = onePoleCoef(hz); }

  // For a filter that runs at rate Hz.
  void setAt(float hz, float rate) { a = onePoleCoefAt(hz, rate); }

  CV_INLINE float lp(float x) {
    y += a * (x - y);
    return y;
  }

  CV_INLINE float hp(float x) { return x - lp(x); }

  void clear() { y = 0.0f; }
};

// Corner of the DC blocker on an engine's wet path.
constexpr float kWetDcBlockHz = 5.0f;

// Coefficient of a one-pole high-pass with its corner at hz, from the series of 1 - exp(-x). It
// lands within 1.2e-7 relative of the exact value at 100 Hz and below. Unlike onePoleCoef it
// does not clamp the corner to kFilterMinHz, so it can sit below that.
constexpr float dcBlockCoef(float hz) {
  const float x = kTwoPi * hz / kSampleRate;
  return x * (1.0f - x * (0.5f - x * (1.0f / 6.0f)));
}

// One-pole high-pass at kWetDcBlockHz. It removes the DC an engine's wet path can carry, so the
// stages after it do not pump on it. The state starts at zero and settles to the input's mean.
struct DcBlocker {
  float a = dcBlockCoef(kWetDcBlockHz);
  float y = 0.0f;

  CV_INLINE float run(float x) {
    y += a * (x - y);
    return x - y;
  }

  void clear() { y = 0.0f; }
};

// The corner of an RBJ section: the cosine and sine of w0 = 2 pi hz / rate, with hz clamped to the
// filter band. A stage that moves only the gain of a peak or shelf keeps the corner and re-sets the
// gain through setPeakAmp, setLowShelfAmp or setHighShelfAmp.
struct RbjCorner {
  float cw, sw;
};

inline RbjCorner rbjCorner(float hz) {
  const float w0 = kTwoPi * clampf(hz, kFilterMinHz, kFilterMaxHz) / kSampleRate;
  return {cosf(w0), sinf(w0)};
}

// RBJ alpha = sin(w0) / (2 q).
inline float rbjAlpha(float sw, float q) { return sw / (2.0f * q); }

// The amplitude A = 10^(db / 40) that the RBJ peak and shelf forms take.
inline float rbjAmp(float db) { return powf(10.0f, db / 40.0f); }

// Peak Q never goes below this, so a stray tuning value cannot put a pole on the unit circle.
constexpr float kPeakMinQ = 0.1f;

// Shelf slope S runs from kShelfMaxSlope, the Butterworth width, down to kShelfMinSlope, which narrows
// the transition.
constexpr float kShelfMaxSlope = 1.0f;
constexpr float kShelfMinSlope = 0.1f;

// RBJ cookbook biquad in transposed direct form II.
struct Biquad {
  float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
  float z1 = 0.0f, z2 = 0.0f;

  void setLowPass(float hz, float q) {
    const RbjCorner k = rbjCorner(hz);
    const float cw = k.cw;
    const float alpha = rbjAlpha(k.sw, q);
    const float a0 = 1.0f + alpha;
    b0 = 0.5f * (1.0f - cw) / a0;
    b1 = (1.0f - cw) / a0;
    b2 = b0;
    a1 = -2.0f * cw / a0;
    a2 = (1.0f - alpha) / a0;
  }

  void setHighPass(float hz, float q) {
    const RbjCorner k = rbjCorner(hz);
    const float cw = k.cw;
    const float alpha = rbjAlpha(k.sw, q);
    const float a0 = 1.0f + alpha;
    b0 = 0.5f * (1.0f + cw) / a0;
    b1 = -(1.0f + cw) / a0;
    b2 = b0;
    a1 = -2.0f * cw / a0;
    a2 = (1.0f - alpha) / a0;
  }

  void setPeak(float hz, float db, float q) { setPeakAmp(rbjCorner(hz), rbjAmp(db), q); }

  // The peak at a corner and a linear amplitude A.
  void setPeakAmp(const RbjCorner& k, float amp, float q) {
    const float alpha = rbjAlpha(k.sw, fmaxf(q, kPeakMinQ));
    const float a0 = 1.0f + alpha / amp;
    b0 = (1.0f + alpha * amp) / a0;
    b1 = -2.0f * k.cw / a0;
    b2 = (1.0f - alpha * amp) / a0;
    a1 = b1;
    a2 = (1.0f - alpha / amp) / a0;
  }

  void setLowShelf(float hz, float db) { setLowShelfAmp(rbjCorner(hz), rbjAmp(db)); }

  // `slope` is the RBJ shelf slope S. Slope 1, which every other caller gets, takes the Butterworth
  // width directly, so those shelves keep their coefficients exact.
  void setHighShelf(float hz, float db, float slope = kShelfMaxSlope) {
    setHighShelfAmp(rbjCorner(hz), rbjAmp(db), slope);
  }

  void setLowShelfAmp(const RbjCorner& k, float amp) { setShelf(k, amp, -1.0f, kShelfMaxSlope); }

  void setHighShelfAmp(const RbjCorner& k, float amp, float slope = kShelfMaxSlope) {
    setShelf(k, amp, 1.0f, clampf(slope, kShelfMinSlope, kShelfMaxSlope));
  }

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
  void setShelf(const RbjCorner& k, float amp, float sign, float slope) {
    const float cw = k.cw;
    const float alpha = slope == kShelfMaxSlope
                            ? k.sw * kButterworthQ
                            : 0.5f * k.sw * sqrtf((amp + 1.0f / amp) * (1.0f / slope - 1.0f) + 2.0f);
    const float beta = 2.0f * sqrtf(amp) * alpha;
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

// Catmull-Rom read `delay` samples behind `head` in a ring of Len samples. With the head on the newest
// sample, the read is exact for a delay in (1, Len - 2]. The caller keeps it inside [2, Len - 3], so the
// four taps never cross the head. Below 1 sample the taps pull in the oldest sample. Any delay, NaN
// included, indexes inside the ring.
template <int Len>
CV_INLINE float readRing(const float* ring, int head, float delay) {
  constexpr float kIndexLimit = 1.0e9f;  // keeps the cast to int defined; wrapOnce sends it to 0
  const float pos = static_cast<float>(head) - delay;
  const float fl = floorf(pos);
  const int i0 = wrapOnce<Len>(static_cast<int>(fminf(fmaxf(fl, -kIndexLimit), kIndexLimit)));
  return catmullRom(ring[wrapOnce<Len>(i0 - 1)], ring[i0], ring[wrapOnce<Len>(i0 + 1)], ring[wrapOnce<Len>(i0 + 2)],
                    pos - fl);
}

// readRing for a delay that may leave [2, Len - 3]; it clamps to that span first.
template <int Len>
CV_INLINE float readRingClamped(const float* ring, int head, float delay) {
  return readRing<Len>(ring, head, clampf(delay, 2.0f, Len - 3.0f));
}

}  // namespace cv
