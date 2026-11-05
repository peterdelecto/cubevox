#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "engine/common.h"
#include "engine/dsp.h"

// Cheap replacements for libm calls that sit inside per-sample loops, and the gain helper built on
// them. Each function has a test that pins its error against libm in test/fastmath_test.cpp.
// A NaN input gives a finite result from every function that casts to int, so a stray NaN never
// reaches undefined behaviour here.

namespace cv {

constexpr float kLog2E = 1.44269504089f;     // log2(e)
constexpr float kTurn32 = 4294967296.0f;     // one turn of a 32-bit phase counter

// 2^x to 7.5e-8 relative for the float x it is given: a degree-6 polynomial on the
// fraction, then the integer part goes straight into the float exponent. x clamps to +-126, and NaN
// reads as -126.
inline float exp2Fast(float x) {
  x = fminf(fmaxf(x, -126.0f), 126.0f);
  const int r = static_cast<int>(x + 0.5f + 1024.0f) - 1024;  // floor(x + 0.5)
  const float f = x - static_cast<float>(r);                   // -0.5 .. 0.5
  float p = 1.535336188319500e-4f;
  p = p * f + 1.339887440266574e-3f;
  p = p * f + 9.618437357674640e-3f;
  p = p * f + 5.550332471162809e-2f;
  p = p * f + 2.402264791363012e-1f;
  p = p * f + 6.931472028550421e-1f;
  p = p * f + 1.0f;
  const uint32_t bits = static_cast<uint32_t>(r + 127) << 23;
  float scale;
  std::memcpy(&scale, &bits, sizeof scale);
  return p * scale;
}

// log2(x) to under 3e-5 absolute for positive normal x: the exponent field plus a
// degree-5 polynomial on the mantissa in [1, 2). The fit passes through 0 at 1 and
// 1 at 2, so the result is continuous across octaves. Zero, negative and subnormal
// inputs return garbage, so callers keep x above a floor.
inline float log2Fast(float x) {
  uint32_t bits;
  std::memcpy(&bits, &x, sizeof bits);
  const float exponent = static_cast<float>(static_cast<int32_t>(bits >> 23) - 127);
  bits = (bits & 0x007FFFFFu) | 0x3F800000u;  // mantissa with exponent 0: [1, 2)
  float m;
  std::memcpy(&m, &bits, sizeof m);
  const float t = m - 1.0f;
  float p = 4.5149041594e-2f;
  p = p * t - 1.9357568174e-1f;
  p = p * t + 4.1560604039e-1f;
  p = p * t - 7.0909643857e-1f;
  p = p * t + 1.4419170383f;
  return exponent + p * t;
}

constexpr float kDbPerLog2 = 6.02059991328f;  // 20 log10(2): amplitude dB per octave of level
constexpr float kLog2PerDb = 0.16609640474f;  // log2(10) / 20

// 20 log10(x) to under 2e-4 dB for positive normal x, through log2Fast.
inline float dbFast(float x) { return kDbPerLog2 * log2Fast(x); }

// 10^(db / 20) to 7.7e-7 relative on -140 to +24 dB, through exp2Fast. The error is the rounding of
// db times kLog2PerDb. It is for per-sample use; dbToGain in dsp.h is the exact form for a value
// computed once a block or on a setting change.
inline float dbToGainFast(float db) { return exp2Fast(db * kLog2PerDb); }

// A linear gain from a level in dB, worked out only when the dB moves. 0 dB is exactly 1.
class DbGain {
 public:
  void set(float db) {
    if (db == db_) return;
    db_ = db;
    gain_ = dbToGainFast(db);
  }
  float gain() const { return gain_; }

 private:
  float db_ = 0.0f;
  float gain_ = 1.0f;
};

// sin(2 pi phase / 2^32) from a 32-bit turn counter. The phase folds to a
// quarter turn, then sin(pi t) is the odd Taylor series through t^11 on
// |t| <= 0.5. The first dropped term is below 1e-7, so absolute error stays
// under 1e-6 with float rounding (1.6e-7 measured). A quarter turn, 2^30, returns 0.99999994 and not 1.
CV_INLINE float sinTurns(uint32_t phase) {
  const int32_t p = static_cast<int32_t>(phase);
  float t = static_cast<float>(p) * (1.0f / 2147483648.0f);

  // Integer tests avoid a float compare stall. They differ from t > 0.5f only
  // where t rounds to exactly +-0.5, and there the fold returns the same value.
  if (p > 0x40000000) t = 1.0f - t;
  else if (p < -0x40000000) t = -1.0f - t;

  constexpr float c1 = kPi;
  constexpr float c3 = -(kPi * kPi * kPi) / 6.0f;
  constexpr float c5 = (kPi * kPi * kPi * kPi * kPi) / 120.0f;
  constexpr float c7 = -(kPi * kPi * kPi * kPi * kPi * kPi * kPi) / 5040.0f;
  constexpr float c9 = (kPi * kPi * kPi * kPi * kPi * kPi * kPi * kPi * kPi) / 362880.0f;
  constexpr float c11 =
      -(kPi * kPi * kPi * kPi * kPi * kPi * kPi * kPi * kPi * kPi * kPi) / 39916800.0f;
  const float t2 = t * t;
  return t * (c1 + t2 * (c3 + t2 * (c5 + t2 * (c7 + t2 * (c9 + t2 * c11)))));
}

// tanh to under 3e-7 absolute: Eigen's odd 13/6 rational form. Inputs at or
// beyond +-9 return exactly +-1; the polynomial input clamps at +-7.9053111,
// where the rational already rounds to +-1. The form is odd, so
// tanhFast(-x) == -tanhFast(x) bit for bit.
CV_INLINE float tanhFast(float x) {
  if (x >= 9.0f) return 1.0f;
  if (x <= -9.0f) return -1.0f;
  constexpr float kClamp = 7.9053111f;
  x = x < -kClamp ? -kClamp : (x > kClamp ? kClamp : x);
  const float x2 = x * x;
  float p = -2.76076847742355e-16f;
  p = p * x2 + 2.00018790482477e-13f;
  p = p * x2 + -8.60467152213735e-11f;
  p = p * x2 + 5.12229709037114e-08f;
  p = p * x2 + 1.48572235717979e-05f;
  p = p * x2 + 6.37261928875436e-04f;
  p = p * x2 + 4.89352455891786e-03f;
  p *= x;
  float q = 1.19825839466702e-06f;
  q = q * x2 + 1.18534705686654e-04f;
  q = q * x2 + 2.26843463243900e-03f;
  q = q * x2 + 4.89352518554385e-03f;
  return p / q;
}

// sin and cos of x radians for |x| <= 16 pi, to float rounding, with no libm.
// x folds onto r within pi / 4 of q quarter turns, with pi / 2 in three parts
// so the fold itself rounds nothing (Cody and Waite). The Taylor series through
// r^9 and r^10 finish it; the first dropped terms are under 3e-9. Under 0.14 rad
// q is 0, r is x, and only the series runs. x clamps to +-16 pi, so NaN reads as -16 pi.
CV_INLINE void sinCosFast(float x, float& s, float& c) {
  constexpr float kMaxArg = 16.0f * kPi;
  constexpr float kTwoOverPi = 0.636619772f;
  x = fminf(fmaxf(x, -kMaxArg), kMaxArg);
  constexpr float kQuarterA = 1.5703125f;
  constexpr float kQuarterB = 4.837512969970703125e-4f;
  constexpr float kQuarterC = 7.549789948768648e-8f;
  const int q = static_cast<int>(x * kTwoOverPi + (x < 0.0f ? -0.5f : 0.5f));
  const float qf = static_cast<float>(q);
  const float r = ((x - qf * kQuarterA) - qf * kQuarterB) - qf * kQuarterC;
  const float r2 = r * r;
  constexpr float s3 = -1.0f / 6.0f, s5 = 1.0f / 120.0f, s7 = -1.0f / 5040.0f, s9 = 1.0f / 362880.0f;
  constexpr float c4 = 1.0f / 24.0f, c6 = -1.0f / 720.0f, c8 = 1.0f / 40320.0f, c10 = -1.0f / 3628800.0f;
  const float sr = r + r * r2 * (s3 + r2 * (s5 + r2 * (s7 + r2 * s9)));
  const float cr = 1.0f + r2 * (-0.5f + r2 * (c4 + r2 * (c6 + r2 * (c8 + r2 * c10))));
  switch (q & 3) {
    case 0: s = sr; c = cr; break;
    case 1: s = cr; c = -sr; break;
    case 2: s = -sr; c = -cr; break;
    default: s = -cr; c = sr; break;
  }
}

// sin and cos of a share of the quarter turn, share in [0, 1]. They land exactly on 0 and 1 at both
// ends, which sinCosFast would miss by a hair. An equal-power crossfade takes its two gains from it.
CV_INLINE void quarterTurn(float share, float& s, float& c) {
  if (share <= 0.0f) {
    s = 0.0f;
    c = 1.0f;
  } else if (share >= 1.0f) {
    s = 1.0f;
    c = 0.0f;
  } else {
    sinCosFast(share * kHalfPi, s, c);
  }
}

// Seed of a Hann window by rotation: the cosine and sine of the phase (c, s) and of the per-sample
// step (dc, ds). The caller turns (c, s) by (dc, ds) once a sample, one complex multiply in place of a
// cosine, and the window is 0.5 - 0.5 * c. init() runs sinCosFast twice and no libm. The turn drifts
// about 3e-9 a sample, so callers re-init at each block.
struct HannRotor {
  float c = 1.0f;
  float s = 0.0f;
  float dc = 1.0f;
  float ds = 0.0f;

  void init(float phase, float step) {
    sinCosFast(phase, s, c);
    sinCosFast(step, ds, dc);
  }
};

}  // namespace cv
