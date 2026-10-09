#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "engine/common.h"

// Cheap replacements for libm calls that sit inside per-sample loops. Each has
// a test in test/fastmath_test.cpp that pins its error.

namespace cv {

// 2^x to about 1e-7 relative: a degree-6 polynomial on the fraction, then the
// integer part goes straight into the float exponent. x clamps to +-126.
inline float exp2Fast(float x) {
  x = x < -126.0f ? -126.0f : (x > 126.0f ? 126.0f : x);
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

// sin(2 pi phase / 2^32) from a 32-bit turn counter. The phase folds to a
// quarter turn, then sin(pi t) is the odd Taylor series through t^11 on
// |t| <= 0.5. The first dropped term is below 1e-7, so absolute error stays
// under 1e-6 with float rounding.
CV_INLINE float sinTurns(uint32_t phase) {
  const int32_t p = static_cast<int32_t>(phase);
  float t = static_cast<float>(p) * (1.0f / 2147483648.0f);

  // Integer tests avoid a float compare stall. They differ from t > 0.5f only
  // where t rounds to exactly +-0.5, and there the fold returns the same value.
  if (p > 0x40000000) t = 1.0f - t;
  else if (p < -0x40000000) t = -1.0f - t;

  constexpr float kPi = 3.14159265358979323846f;
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

// tanh to about 1e-6 absolute: Eigen's odd 13/6 rational form. Inputs at or
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

// Hann window by rotation: one complex multiply per sample in place of a
// cosine. init() costs two sines and two cosines; drift over a grain is
// about n * 1e-7, so callers re-init at each grain or each block.
struct HannRotor {
  float c = 1.0f;
  float s = 0.0f;
  float dc = 1.0f;
  float ds = 0.0f;

  void init(float phase, float step) {
    c = cosf(phase);
    s = sinf(phase);
    dc = cosf(step);
    ds = sinf(step);
  }

  // 0.5 * (1 - cos(phase)) at the current phase.
  float window() const { return 0.5f * (1.0f - c); }

  void advance() {
    const float c2 = c * dc - s * ds;
    s = s * dc + c * ds;
    c = c2;
  }
};

}  // namespace cv
