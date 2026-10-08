#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

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
