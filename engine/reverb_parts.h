// The Schroeder allpass, the interpolated delay line and the wobble LFO below are the delay and
// allpass code of the CHASM port, shared with Spring. They carry its notice.
//
//   Copyright (c) 2024 Piotr Zapart / www.hexefx.com
//
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the "Software"),
//   to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense,
//   and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//   The above copyright notice and this permission notice shall be included in
//   all copies or substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/smooth.h"

// Parts the two reverbs share: the Schroeder allpass, the interpolated delay line, the phase step of
// a wobble LFO and the sine table behind it, and the Dwell drive.

namespace cv {
namespace reverb_parts {

// Phase accumulator step per Hz of LFO rate, in 1/2^32 turns per sample.
constexpr float kPhasePerHz = 4294967296.0f / static_cast<float>(kSampleRate);

// Loop feedback never exceeds this, so a tuning value cannot make a loop unstable.
constexpr float kFeedbackMax = 0.97f;

// A knob law: piecewise linear through lo, mid and hi at the knob's 0, 0.5 and 1.
struct KnobCurve {
  float lo, mid, hi;
  constexpr float at(float t) const {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t < 0.5f ? lo + (mid - lo) * 2.0f * t : mid + (hi - mid) * (2.0f * t - 1.0f);
  }
};

// One Schroeder allpass step on the state z with coefficient k: out = z + k*in, z = in - k*out.
CV_INLINE float allpassStep(float& z, float k, float in) {
  const float out = z + k * in;
  z = in - k * out;
  return out;
}

// Schroeder allpass of length N with coefficient k.
template <int N>
struct Allpass {
  std::array<float, N> buf{};
  int pos = 0;

  float run(float in, float k) {
    const float out = allpassStep(buf[static_cast<size_t>(pos)], k, in);
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

// A reverb evaluates its wobble at the edges of each block and interpolates linearly between. The
// sine's curvature bounds the read position's error at depth / 2 * (2 pi rate)^2 * T^2 / 8 samples for
// a block of T seconds.
constexpr float kMaxWobbleErr = 0.05f;  // samples
constexpr float wobbleError(float depth, float rateHz) {
  const float w = kTwoPi * rateHz;
  const float t = static_cast<float>(kBlock) / static_cast<float>(kSampleRate);
  return 0.5f * depth * w * w * t * t / 8.0f;
}

// Dwell drives a soft clip ahead of the loops. The drive runs from 1 to dwellDrive as dwell runs from
// 0 to 1, and the level compensation takes the drive to the power -dwellComp so the loops see a
// similar level. The terms call libm, so they belong to a tuning change and not to a sample.
constexpr float kMaxDwellDrive = 64.0f;  // dwellDrive clamps to 1..this
constexpr float kMaxDwellComp = 2.0f;    // dwellComp clamps to 0..this

struct DwellGain {
  float drive, comp;
};

inline DwellGain dwellGain(float dwell, float dwellDrive, float dwellComp) {
  const float d = smooth::clamp01(dwell);
  const float logDrive = logf(clampf(dwellDrive, 1.0f, kMaxDwellDrive));
  return {expf(logDrive * d), expf(-clampf(dwellComp, 0.0f, kMaxDwellComp) * logDrive * d)};
}

}  // namespace reverb_parts
}  // namespace cv
