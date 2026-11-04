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
// line, the knob law, the phase step of a wobble LFO and the sine table behind it, and the Dwell drive.

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

// Headroom for both reverbs' input drive, set so a 0 dBFS voice at each engine's default dwell of
// 0.2 enters the curve at about a third of its knee. Chasm's drive is 8^0.2 = 1.52, which is 0.34
// of the knee. Spring's is 4^0.2 = 1.32, which is 0.29.
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

// One turn of sine in 256 steps, plus the wrap point: sinf(i * 2 pi / 256) as float literals, which
// dsp_test pins to within one ulp of libm. The table sits in flash and costs no start-up code.
inline constexpr std::array<float, 257> kSine = {
    0.0f, 0.024541229f, 0.0490676761f, 0.0735645667f, 0.0980171412f, 0.122410677f,
    0.146730483f, 0.170961902f, 0.195090324f, 0.219101235f, 0.242980197f, 0.266712785f,
    0.290284693f, 0.313681751f, 0.336889863f, 0.359895051f, 0.382683456f, 0.40524134f,
    0.427555084f, 0.449611336f, 0.471396744f, 0.492898226f, 0.514102757f, 0.534997582f,
    0.555570245f, 0.575808227f, 0.59569931f, 0.615231633f, 0.634393334f, 0.653172851f,
    0.671558976f, 0.689540565f, 0.707106769f, 0.724247098f, 0.74095118f, 0.757208884f,
    0.773010433f, 0.78834641f, 0.803207517f, 0.817584813f, 0.831469655f, 0.84485358f,
    0.85772866f, 0.870086968f, 0.881921291f, 0.893224299f, 0.903989315f, 0.914209783f,
    0.923879504f, 0.932992816f, 0.941544056f, 0.949528217f, 0.956940353f, 0.963776052f,
    0.970031261f, 0.975702107f, 0.98078531f, 0.985277653f, 0.989176512f, 0.992479563f,
    0.99518472f, 0.997290432f, 0.99879545f, 0.999698818f, 1.0f, 0.999698818f,
    0.99879545f, 0.997290432f, 0.99518472f, 0.992479503f, 0.989176512f, 0.985277653f,
    0.980785251f, 0.975702107f, 0.970031261f, 0.963776052f, 0.956940293f, 0.949528158f,
    0.941544056f, 0.932992816f, 0.923879504f, 0.914209723f, 0.903989315f, 0.893224299f,
    0.881921232f, 0.870087028f, 0.857728601f, 0.84485352f, 0.831469536f, 0.817584813f,
    0.803207517f, 0.78834635f, 0.773010492f, 0.757208824f, 0.740951061f, 0.724246979f,
    0.707106769f, 0.689540505f, 0.671558857f, 0.653172851f, 0.634393275f, 0.615231514f,
    0.595699131f, 0.575808167f, 0.555570185f, 0.534997463f, 0.514102757f, 0.492898136f,
    0.471396625f, 0.449611366f, 0.427555054f, 0.405241221f, 0.382683277f, 0.359895051f,
    0.336889803f, 0.313681602f, 0.290284723f, 0.266712725f, 0.242980078f, 0.219101071f,
    0.195090309f, 0.170961812f, 0.146730334f, 0.1224107f, 0.0980170965f, 0.0735644475f,
    0.0490674861f, 0.0245412104f, -8.74227766e-08f, -0.0245413855f, -0.0490676612f, -0.0735646188f,
    -0.0980172679f, -0.122410871f, -0.146730497f, -0.170961991f, -0.195090488f, -0.219101235f,
    -0.242980242f, -0.266712904f, -0.290284872f, -0.313681781f, -0.336889952f, -0.3598952f,
    -0.382683456f, -0.4052414f, -0.427555233f, -0.449611515f, -0.471396774f, -0.492898285f,
    -0.514102876f, -0.534997642f, -0.555570304f, -0.575808346f, -0.59569931f, -0.615231633f,
    -0.634393394f, -0.65317297f, -0.671558976f, -0.689540625f, -0.707106888f, -0.724247098f,
    -0.74095118f, -0.757208765f, -0.773010433f, -0.788346469f, -0.803207576f, -0.817584932f,
    -0.831469774f, -0.844853759f, -0.857728541f, -0.870086968f, -0.881921291f, -0.893224359f,
    -0.903989375f, -0.914209843f, -0.923879683f, -0.932992756f, -0.941544056f, -0.949528217f,
    -0.956940353f, -0.963776112f, -0.970031321f, -0.975702226f, -0.980785251f, -0.985277653f,
    -0.989176512f, -0.992479563f, -0.99518472f, -0.997290492f, -0.99879545f, -0.999698818f,
    -1.0f, -0.999698818f, -0.99879545f, -0.997290432f, -0.99518472f, -0.992479503f,
    -0.989176512f, -0.985277653f, -0.980785251f, -0.975702107f, -0.970031202f, -0.963775992f,
    -0.956940234f, -0.949528217f, -0.941544056f, -0.932992756f, -0.923879445f, -0.914209664f,
    -0.903989136f, -0.89322412f, -0.881921291f, -0.870086968f, -0.857728541f, -0.844853461f,
    -0.831469476f, -0.817584634f, -0.803207576f, -0.78834641f, -0.773010433f, -0.757208765f,
    -0.740951002f, -0.724246919f, -0.707106531f, -0.689540625f, -0.671558976f, -0.653172791f,
    -0.634393156f, -0.615231454f, -0.595699072f, -0.575807929f, -0.555570304f, -0.534997642f,
    -0.514102697f, -0.492898077f, -0.471396536f, -0.449611068f, -0.427554786f, -0.40524137f,
    -0.382683426f, -0.359894961f, -0.336889714f, -0.313681543f, -0.290284395f, -0.266712397f,
    -0.242980227f, -0.21910122f, -0.195090234f, -0.170961723f, -0.146730244f, -0.122410372f,
    -0.0980167687f, -0.0735645965f, -0.0490676388f, -0.0245411228f, 1.74845553e-07f};

// Unipolar sine 0..1 at the phase accumulator acc, offset by phase8 in 1/256 turns.
inline float lfo(uint32_t acc, uint32_t phase8) {
  const uint32_t i0 = ((acc >> 24) + phase8) & 0xFFu;
  const float fr = static_cast<float>(acc & 0x00FFFFFFu) * (1.0f / 16777216.0f);
  const float s = kSine[i0] + fr * (kSine[i0 + 1u] - kSine[i0]);
  return s * 0.5f + 0.5f;
}

// A reverb evaluates its wobble at the edges of each block and interpolates linearly between. The
// sine's curvature bounds the read position's error at depth / 2 * (2 pi rate)^2 * T^2 / 8 samples for
// a block of T seconds, and the table's own linear interpolation adds depth * 3.8e-5 (its (2 pi / 256)^2
// / 16 bound on the 0..1 swing).
constexpr float kMaxWobbleErr = 0.05f;  // samples
constexpr float wobbleError(float depth, float rateHz) {
  const float w = kTwoPi * rateHz;
  const float t = static_cast<float>(kBlock) / static_cast<float>(kSampleRate);
  return 0.5f * depth * w * w * t * t / 8.0f + depth * 3.8e-5f;
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
