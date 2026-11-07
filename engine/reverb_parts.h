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
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Parts the two reverbs share: the input soft clip, the Schroeder allpass, the interpolated delay
// line, the knob law, the phase step of a wobble LFO and its sine.

namespace cv {

constexpr float kClipMax = 3.0f;

// Padé tanh, clamped where it meets the rail.
inline float softClip(float x) {
  x = x < -kClipMax ? -kClipMax : (x > kClipMax ? kClipMax : x);
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// The same curve with headroom h: unity gain below the knee, which sits h times
// higher, so a reverb's input drive saturates only near the top of its range. With a constant h the
// compiler folds 1 / h, so the input scale costs a multiply.
inline float softClipHeadroom(float x, float h) { return h * softClip(x * (1.0f / h)); }

// Headroom for both reverbs' input drive, set so a 0 dBFS voice enters the curve at about a third
// of its knee. Chasm's drive is 1.52, which is 0.34 of the knee. Spring's is 1.32, which is 0.29.
constexpr float kReverbClipHeadroom = 4.5f;

namespace reverb_parts {

// Phase accumulator step per Hz of LFO rate, in 1/2^32 turns per sample.
constexpr float kPhasePerHz = kTurn32 / static_cast<float>(kSampleRate);

// Loop feedback never exceeds this, so a tuning value cannot make a loop unstable.
constexpr float kFeedbackMax = 0.97f;

// A knob law: piecewise linear through lo, mid and hi at the knob's 0, 0.5 and 1.
struct KnobCurve {
  float lo, mid, hi;
  constexpr float at(float t) const {
    t = smooth::clamp01(t);
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

// Delay line of length N. read(adv) shortens the line by adv samples, interpolated linearly. adv is
// right on [0, N - 1] and clamps to that span, so even a NaN indexes inside the line.
template <int N>
struct Delay {
  std::array<float, N> buf{};
  int pos = 0;

  float read(float adv) const {
    adv = clampf(adv, 0.0f, static_cast<float>(N - 1));
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

// Unipolar sine 0..1 at the phase accumulator acc, offset by phase8 in 1/256 turns.
inline float lfo(uint32_t acc, uint32_t phase8) { return sinTurns(acc + (phase8 << 24)) * 0.5f + 0.5f; }

// The fastest wobble either reverb takes, in Hz.
constexpr float kMaxWobbleHz = 10.0f;

// A reverb evaluates its wobble at the edges of each block and interpolates linearly between. The
// sine's curvature bounds the read position's error at depth / 2 * (2 pi rate)^2 * T^2 / 8 samples for
// a block of T seconds, and sinTurns adds depth * 1e-6 (its absolute error bound on the 0..1 swing).
constexpr float kMaxWobbleErr = 0.05f;  // samples
constexpr float wobbleError(float depth, float rateHz) {
  const float w = kTwoPi * rateHz;
  const float t = static_cast<float>(kBlock) / static_cast<float>(kSampleRate);
  return 0.5f * depth * w * w * t * t / 8.0f + depth * 1e-6f;
}

}  // namespace reverb_parts
}  // namespace cv
