#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/smooth.h"

// CHASM reverb: one diffuse allpass feedback loop with a downward chirp on its
// output. Float port of DrumSynthV3 AudioEffectCavernV3, the mono port of
// hexefx_audiolib_F32 AudioEffectSpringReverb_F32.
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
//
// Loop: dly1 -> allpass 1a..1d -> dly2 -> allpass 2a..2d -> dly1. Each delay
// read is scaled by the feedback, then passes a treble shelf and a one-pole
// high-pass. The two filtered taps feed four chirp stages of 16 short
// allpasses each; lows take longest through them, so the boing runs down.
// DECAY sets the feedback and slides the bass cut down over its upper half.
// WOBBLE moves both delay read heads with a sine LFO, 90 degrees apart.
// Delay and allpass lengths are the 44.1 kHz originals scaled to 48 kHz;
// chirp lengths are not scaled. Output is wet only, at wetDb.

namespace cv {

struct ChasmTuning {
  float timeLo = 0.35f, timeHi = 0.86f;  // feedback at DECAY 0 / 1
  float trebleLossHz = 3000.0f;
  float loopTrebleCut = 1.0f;            // 1 = off
  float inputTrebleCut = 0.95f;
  float bassCutHz = 200.0f, bassCutHzTop = 100.0f;
  float wobbleDepthMax = 64.0f;          // samples
  float wobbleRateLo = 0.5f, wobbleRateHi = 7.0f;
  float inputTrim = 0.5f;
  float wobbleLevelDb = 2.0f;            // wet lift, ramps in over WOBBLE 0..0.25
  float wetDb = 17.4f;  // trim; level rule at MIX 0.5
};

struct ChasmParams {
  float decay = 0.30f;  // knob 1; vocal default
  float wobble = 0.15f; // knob 2; vocal default
  ChasmTuning tuning;
};

namespace chasm_detail {

constexpr float kTwoPi = 6.28318530717958647692f;

// 44.1 kHz length rescaled to the engine rate, rounded.
constexpr int scaled(int len) { return (len * kSampleRate + 22050) / 44100; }

constexpr int kAllp1A = scaled(224), kAllp1B = scaled(420);
constexpr int kAllp1C = scaled(856), kAllp1D = scaled(1089);
constexpr int kAllp2A = scaled(156), kAllp2B = scaled(478);
constexpr int kAllp2C = scaled(956), kAllp2D = scaled(1289);
constexpr int kDly1 = scaled(1945), kDly2 = scaled(1363);
constexpr int kChirpDepth = 16;

constexpr float kTimeMax = 0.97f;  // stability ceiling
constexpr float kLoopK = 0.6f;     // loop allpass coefficient
constexpr std::array<float, 4> kChirpK = {-0.7f, -0.65f, -0.6f, -0.5f};

// Read heads advance by at most this much, so they stay inside the shorter line.
constexpr float kWobbleCap = static_cast<float>(kDly2 / 2);
constexpr float kPhasePerHz = 4294967296.0f / static_cast<float>(kSampleRate);

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// One-pole coefficient for a corner at hz.
inline float onePole(float hz) {
  if (hz < 20.0f) hz = 20.0f;
  const float f = 1.0f - expf(-kTwoPi * hz / static_cast<float>(kSampleRate));
  return f > 0.99f ? 0.99f : f;
}

// Schroeder allpass, out = buf + k*in, buf = in - k*out.
template <int N>
struct Allpass {
  std::array<float, N> buf{};
  int pos = 0;

  float run(float in) {
    const float out = buf[static_cast<size_t>(pos)] + kLoopK * in;
    buf[static_cast<size_t>(pos)] = in - kLoopK * out;
    if (++pos >= N) pos = 0;
    return out;
  }

  void clear() {
    buf.fill(0.0f);
    pos = 0;
  }
};

// Loop delay. read(adv) shortens the line by adv samples, interpolated.
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

// kChirpDepth allpass units of length L in series, cycling the four coefficients.
template <int L>
struct ChirpStage {
  std::array<float, kChirpDepth * L> buf{};
  int pos = 0;

  float run(float in) {
    for (int u = 0; u < kChirpDepth; ++u) {
      const float k = kChirpK[static_cast<size_t>(u & 3)];
      float& z = buf[static_cast<size_t>(u * L + pos)];
      const float out = z + k * in;
      z = in - k * out;
      in = out;
    }
    if (++pos >= L) pos = 0;
    return in;
  }

  void clear() {
    buf.fill(0.0f);
    pos = 0;
  }
};

}  // namespace chasm_detail

class Chasm {
 public:
  Chasm() {
    for (int i = 0; i < 257; ++i)
      sine_[static_cast<size_t>(i)] =
          sinf(static_cast<float>(i) * (chasm_detail::kTwoPi / 256.0f));
    reset();
  }

  void reset() {
    a1a_.clear();
    a1b_.clear();
    a1c_.clear();
    a1d_.clear();
    a2a_.clear();
    a2b_.clear();
    a2c_.clear();
    a2d_.clear();
    dly1_.clear();
    dly2_.clear();
    c1_.clear();
    c2_.clear();
    c3_.clear();
    c4_.clear();
    lpIn_ = lp1_ = lp2_ = hp1_ = hp2_ = 0.0f;
    lfoAcc_ = 0u;
    decay_ = wobble_ = inputGain_ = 0.0f;
    valid_ = false;
  }

  // Wet only; the caller mixes. n <= kBlock.
  void process(const float* in, float* wet, int n, const ChasmParams& p) {
    using namespace chasm_detail;
    const ChasmTuning& t = p.tuning;
    const float decayTarget = smooth::clamp01(p.decay);
    const float wobbleTarget = smooth::clamp01(p.wobble);
    if (!valid_) {
      decay_ = decayTarget;
      wobble_ = wobbleTarget;
    }

    // Block constants. Bass cut and wobble level follow the knobs per block.
    const float a = smooth::coef(smooth::kSmoothSec);
    const float timeHi = clampf(t.timeHi, 0.0f, kTimeMax);
    const float timeLo = clampf(t.timeLo, 0.0f, timeHi);
    const float lpF = onePole(t.trebleLossHz);
    const float loopCut = clampf(t.loopTrebleCut, 0.0f, 1.0f);
    const float inCut = t.inputTrebleCut;
    float bassHz = t.bassCutHz;
    if (decay_ > 0.5f) bassHz *= powf(t.bassCutHzTop / t.bassCutHz, (decay_ - 0.5f) * 2.0f);
    const float hpF = onePole(bassHz);
    const float depthMax = clampf(t.wobbleDepthMax, 0.0f, kWobbleCap);
    const float rateLo = t.wobbleRateLo, rateSpan = t.wobbleRateHi - t.wobbleRateLo;
    const float trim = t.inputTrim;
    // Interpolated reads dull the tail once WOBBLE moves; the lift restores its level.
    const float lift = t.wobbleLevelDb * (wobble_ < 0.25f ? wobble_ * 4.0f : 1.0f);
    const float wetGain = powf(10.0f, (t.wetDb + lift) / 20.0f);

    // Input gain walks down as feedback rises.
    const auto gainFor = [](float time) { return 0.5f - 0.3f * (time / kTimeMax); };
    if (!valid_) {
      inputGain_ = gainFor(timeLo + decay_ * (timeHi - timeLo));
      valid_ = true;
    }

    for (int i = 0; i < n; ++i) {
      decay_ = smooth::step(decay_, decayTarget, a);
      wobble_ = smooth::step(wobble_, wobbleTarget, a);
      const float time = timeLo + decay_ * (timeHi - timeLo);
      const float depth = wobble_ * wobble_ * depthMax;
      lfoAcc_ += static_cast<uint32_t>((rateLo + wobble_ * rateSpan) * kPhasePerHz);
      inputGain_ += (gainFor(time) - inputGain_) * 0.25f;

      const float x = shelf(in[i] * trim * inputGain_, lpIn_, lpF, inCut);
      const float adv1 = lfo(0u) * depth;
      const float adv2 = lfo(64u) * depth;

      float acc = dly1_.read(adv1) * time;
      const float out1 = bassCut(shelf(acc, lp1_, lpF, loopCut), hp1_, hpF);
      acc = a1d_.run(a1c_.run(a1b_.run(a1a_.run(out1))));

      const float d2 = dly2_.read(adv2);
      dly2_.write(acc + x);
      const float out2 = bassCut(shelf(d2 * time, lp2_, lpF, loopCut), hp2_, hpF);
      acc = a2d_.run(a2c_.run(a2b_.run(a2a_.run(out2))));
      dly1_.write(acc + x);

      const float sig = c4_.run(c3_.run(c2_.run(c1_.run(out1 + out2))));
      wet[i] = sig * wetGain;
    }
  }

 private:
  // out = lp + cut*(in - lp); cut 1 passes, cut 0 is the full low-pass.
  static float shelf(float in, float& lp, float f, float cut) {
    lp += (in - lp) * f;
    return lp + cut * (in - lp);
  }

  static float bassCut(float in, float& lp, float f) {
    lp += (in - lp) * f;
    return in - lp;
  }

  // Unipolar sine 0..1; phase8 offsets in 1/256 turns.
  float lfo(uint32_t phase8) const {
    const uint32_t i0 = ((lfoAcc_ >> 24) + phase8) & 0xFFu;
    const float fr = static_cast<float>(lfoAcc_ & 0x00FFFFFFu) * (1.0f / 16777216.0f);
    const float s = sine_[i0] + fr * (sine_[i0 + 1u] - sine_[i0]);
    return s * 0.5f + 0.5f;
  }

  chasm_detail::Allpass<chasm_detail::kAllp1A> a1a_;
  chasm_detail::Allpass<chasm_detail::kAllp1B> a1b_;
  chasm_detail::Allpass<chasm_detail::kAllp1C> a1c_;
  chasm_detail::Allpass<chasm_detail::kAllp1D> a1d_;
  chasm_detail::Allpass<chasm_detail::kAllp2A> a2a_;
  chasm_detail::Allpass<chasm_detail::kAllp2B> a2b_;
  chasm_detail::Allpass<chasm_detail::kAllp2C> a2c_;
  chasm_detail::Allpass<chasm_detail::kAllp2D> a2d_;
  chasm_detail::Delay<chasm_detail::kDly1> dly1_;
  chasm_detail::Delay<chasm_detail::kDly2> dly2_;
  chasm_detail::ChirpStage<3> c1_;
  chasm_detail::ChirpStage<5> c2_;
  chasm_detail::ChirpStage<6> c3_;
  chasm_detail::ChirpStage<7> c4_;
  std::array<float, 257> sine_{};
  float lpIn_ = 0.0f, lp1_ = 0.0f, lp2_ = 0.0f, hp1_ = 0.0f, hp2_ = 0.0f;
  uint32_t lfoAcc_ = 0u;
  float decay_ = 0.0f, wobble_ = 0.0f, inputGain_ = 0.0f;
  bool valid_ = false;
};

}  // namespace cv
