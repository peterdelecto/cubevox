#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"

// Parts the two reverbs share: the Schroeder allpass, the interpolated delay line, the phase step of
// a wobble LFO and the sine table behind it.

namespace cv {
namespace reverb_parts {

// Phase accumulator step per Hz of LFO rate, in 1/2^32 turns per sample.
constexpr float kPhasePerHz = 4294967296.0f / static_cast<float>(kSampleRate);

// Schroeder allpass of length N with coefficient k, out = buf + k*in, buf = in - k*out.
template <int N>
struct Allpass {
  std::array<float, N> buf{};
  int pos = 0;

  float run(float in, float k) {
    const float out = buf[static_cast<size_t>(pos)] + k * in;
    buf[static_cast<size_t>(pos)] = in - k * out;
    if (++pos >= N) pos = 0;
    return out;
  }

  void clear() {
    buf.fill(0.0f);
    pos = 0;
  }
};

// Delay line of length N. read(adv) shortens the line by adv samples, interpolated.
template <int N>
struct Delay {
  std::array<float, N> buf{};
  int pos = 0;

  float read(float adv) const {
    const int whole = static_cast<int>(adv);
    const float fr = adv - static_cast<float>(whole);
    int a = pos + whole;
    if (a >= N) a -= N;
    const int b = a + 1 >= N ? 0 : a + 1;
    return buf[static_cast<size_t>(a)] * (1.0f - fr) + buf[static_cast<size_t>(b)] * fr;
  }

  void write(float x) {
    buf[static_cast<size_t>(pos)] = x;
    if (++pos >= N) pos = 0;
  }

  void clear() {
    buf.fill(0.0f);
    pos = 0;
  }
};

// One turn of sine in 256 steps, plus the wrap point.
inline std::array<float, 257> makeSineTable() {
  std::array<float, 257> t{};
  for (size_t i = 0; i < t.size(); ++i) t[i] = sinf(static_cast<float>(i) * (kTwoPi / 256.0f));
  return t;
}

// Built once at start-up, before any reverb runs. The reverbs read it in process() only.
inline const std::array<float, 257> kSine = makeSineTable();

// Unipolar sine 0..1 at the phase accumulator acc, offset by phase8 in 1/256 turns.
inline float lfo(uint32_t acc, uint32_t phase8) {
  const uint32_t i0 = ((acc >> 24) + phase8) & 0xFFu;
  const float fr = static_cast<float>(acc & 0x00FFFFFFu) * (1.0f / 16777216.0f);
  const float s = kSine[i0] + fr * (kSine[i0 + 1u] - kSine[i0]);
  return s * 0.5f + 0.5f;
}

}  // namespace reverb_parts
}  // namespace cv
