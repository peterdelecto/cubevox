// Float port of DrumSynthV3 AudioEffectCavernV3, the mono port of hexefx_audiolib_F32
// AudioEffectSpringReverb_F32.
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
#include <cstring>
#include <type_traits>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/reverb_parts.h"
#include "engine/smooth.h"

// Chasm Reverb: one diffuse allpass feedback loop with a downward chirp on its output.
//
// The loop runs dly1 -> allpass 1a, 1c -> dly2 -> allpass 2a, 2c -> dly1. Each delay read is scaled
// by the feedback and passes a one-pole high-pass; the loop has no treble filter. The two filtered
// reads add and feed four chirp stages of 8 short allpasses each; lows take longest through them,
// so the boing runs down. TIME sets the feedback and slides the bass cut down over its upper
// quarter, and the input gain follows the feedback so the wet level holds across TIME. A sine LFO
// moves both delay read heads, 90 degrees apart. Dwell, a tuning value with no knob, drives a soft
// clip ahead of the loop with a level compensation, as in Spring. Delay and allpass lengths are the
// 44.1 kHz originals scaled to 48 kHz; chirp lengths are not scaled. Output is wet only, at wetDb.
//
// The loop runs a sample at a time into the output buffer. The chirp stages take the loop's output
// and never feed it, so each stage then runs over the whole block in place.

namespace cv {

// TIME sets the loop feedback, piecewise linear through these values at 0, 50 and 100 %. Each is
// the feedback of the loop with eight allpasses, raised to the power 0.627, which is the ratio of the
// lap of this loop to that one, so the tail lasts as long as it did (T60 0.81, 1.04 and 1.82 s).
constexpr reverb_parts::KnobCurve kChasmFeedback{0.564f, 0.650f, 0.805f};

struct ChasmTuning {
  float bassCutHz = 200.0f;                      // loop high-pass; flat up to TIME 75 %
  float bassCutHzTop = 151.57f;                  // and sliding to this at TIME 100 %
  float wobbleDepth = 25.0800629f;               // samples
  float wobbleRateHz = 4.56899977f;
  float inputTrim = 0.5f;
  float dwell = 0.20f;                           // input drive into the soft clip, 0..1
  float dwellDrive = 8.0f, dwellComp = 0.8f;     // input drive at dwell 1; level comp exponent
  float wetDb = 17.7f;                           // level rule on the wet alone at the box's TIME and dwell, the mean of Adam's clips
};

static_assert(std::is_trivially_copyable<ChasmTuning>::value && sizeof(ChasmTuning) == 9 * sizeof(float),
              "the engine compares the tuning bytes to see whether it changed");

struct ChasmParams {
  float feedback = kChasmFeedback.mid;  // set by TIME through applyKnobs
  ChasmTuning tuning;
};

namespace chasm_detail {

// 44.1 kHz length rescaled to the engine rate, rounded.
constexpr int scaled(int len) { return (len * kSampleRate + 22050) / 44100; }

constexpr int kAllp1A = scaled(224), kAllp1C = scaled(856);
constexpr int kAllp2A = scaled(156), kAllp2C = scaled(956);
constexpr int kDly1 = scaled(1945), kDly2 = scaled(1363);
constexpr int kChirpDepth = 8;

using reverb_parts::kFeedbackMax;
constexpr float kLoopK = 0.6f;  // loop allpass coefficient

// The bass cut starts to slide down where TIME reaches 75 % and has slid all the way at the top.
constexpr float kBassSlideFrom = kChasmFeedback.at(0.75f);
constexpr float kBassSlideTo = kChasmFeedback.hi;

// The input gain at the default feedback g, and 1 / (1 - g^2) there, which scales the gain's
// (1 - g^2) factor to 1 at the default.
constexpr float kInputGain = 0.3f;
constexpr float kInvDefaultSpread = 1.0f / (1.0f - kChasmFeedback.mid * kChasmFeedback.mid);

// Each chirp stage's coefficient, for lengths 3, 5, 6 and 7. The fit puts the stages' group delay
// within 5 ms of the sixteen-unit chirp's between 50 Hz and 6 kHz and shortens the boing.
constexpr std::array<float, 4> kChirpK = {-0.883f, -0.812f, -0.777f, -0.743f};

constexpr float kMaxWobble = 64.0f;  // samples
constexpr float kMaxWobbleHz = 10.0f;
constexpr float kMaxWetDb = 36.0f;   // wetDb clamps to plus and minus this
constexpr float kMaxInputTrim = 4.0f;

static_assert(reverb_parts::wobbleError(kMaxWobble, kMaxWobbleHz) <= reverb_parts::kMaxWobbleErr,
              "the widest, fastest wobble must stay within the interpolation's error limit");

using reverb_parts::Allpass;
using reverb_parts::Delay;

#define CV_PRAGMA(x) _Pragma(#x)
#define CV_UNROLL(n) CV_PRAGMA(GCC unroll n)

// kChirpDepth allpass units of length L in series, all with coefficient kChirpK[Stage]. A unit keeps
// L states, one for each sample of its length. The states sit slot-major so one sample's units are a
// contiguous run. runBlock takes G samples at a time, which fall in G different slots (G <= L), so
// their chains are independent and an in-order core overlaps them.
template <int L, int G, int Stage>
struct ChirpStage {
  static_assert(G >= 1 && G <= L, "a group must fit in one length");
  static constexpr float k = kChirpK[static_cast<size_t>(Stage)];
  std::array<float, kChirpDepth * L> buf{};
  int pos = 0;

  void runBlock(float* x, int n) {
    int j = 0;
    for (; j + G <= n; j += G) {
      float y[G];
      float* z[G];
      CV_UNROLL(4)
      for (int m = 0; m < G; ++m) {
        y[m] = x[j + m];
        int slot = pos + m;
        if (slot >= L) slot -= L;
        z[m] = buf.data() + slot * kChirpDepth;
      }
      // The allpass step for G chains in lock step: every load and multiply-add comes before any store,
      // since the compiler cannot tell the G states apart.
      CV_UNROLL(8)
      for (int u = 0; u < kChirpDepth; ++u) {
        float o[G];
        CV_UNROLL(4)
        for (int m = 0; m < G; ++m) o[m] = z[m][u] + k * y[m];
        CV_UNROLL(4)
        for (int m = 0; m < G; ++m) z[m][u] = y[m] - k * o[m];
        CV_UNROLL(4)
        for (int m = 0; m < G; ++m) y[m] = o[m];
      }
      CV_UNROLL(4)
      for (int m = 0; m < G; ++m) x[j + m] = y[m];
      pos += G;
      if (pos >= L) pos -= L;
    }
    for (; j < n; ++j) {
      float y = x[j];
      float* z = buf.data() + pos * kChirpDepth;
      for (int u = 0; u < kChirpDepth; ++u) y = reverb_parts::allpassStep(z[u], k, y);
      x[j] = y;
      if (++pos >= L) pos = 0;
    }
  }

  void clear() {
    buf.fill(0.0f);
    pos = 0;
  }
};

#undef CV_UNROLL
#undef CV_PRAGMA

}  // namespace chasm_detail

class Chasm {
 public:
  Chasm() { reset(); }

  void reset() {
    a1a_.clear();
    a1c_.clear();
    a2a_.clear();
    a2c_.clear();
    dly1_.clear();
    dly2_.clear();
    c1_.clear();
    c2_.clear();
    c3_.clear();
    c4_.clear();
    hp1_.clear();
    hp2_.clear();
    lfoAcc_ = 0u;
    feedback_ = 0.0f;
    hpFeedback_ = 0.0f;
    valid_ = false;
    built_ = false;
    hpValid_ = false;
  }

  // Wet only; the caller mixes. Any n; the engine works in blocks of kBlock.
  void process(const float* in, float* wet, int n, const ChasmParams& p) {
    for (int done = 0; done < n; done += kBlock) {
      const int m = n - done < kBlock ? n - done : kBlock;
      run(in + done, wet + done, m, p);
    }
  }

 private:
  // Everything the loop takes from the tuning, once per tuning change.
  struct Coefs {
    float bassHz, bassHzTop, depth, drive;
    float inScale;  // the dwell compensation, the input trim and the wet gain: the loop and the chirp are linear
    uint32_t lfoStep;
  };

  // Rebuilds the coefficients only when the tuning differs from the last block's, which on the box is
  // the first block alone.
  void refresh(const ChasmTuning& t) {
    if (built_ && std::memcmp(&t, &tuning_, sizeof t) == 0) return;
    coefs_ = coefsFor(t);
    tuning_ = t;
    built_ = true;
    hpValid_ = false;
  }

  static Coefs coefsFor(const ChasmTuning& t) {
    using namespace chasm_detail;
    Coefs c;
    c.bassHz = clampf(t.bassCutHz, kFilterMinHz, kFilterMaxHz);
    c.bassHzTop = clampf(t.bassCutHzTop, kFilterMinHz, kFilterMaxHz);
    const float depth = clampf(t.wobbleDepth, 0.0f, kMaxWobble);
    c.depth = depth;
    c.lfoStep = static_cast<uint32_t>(clampf(t.wobbleRateHz, 0.0f, kMaxWobbleHz) * reverb_parts::kPhasePerHz);
    const float wetGain = dbToGain(clampf(t.wetDb, -kMaxWetDb, kMaxWetDb));
    const reverb_parts::DwellGain dwell = reverb_parts::dwellGain(t.dwell, t.dwellDrive, t.dwellComp);
    c.drive = dwell.drive;
    c.inScale = dwell.comp * clampf(t.inputTrim, 0.0f, kMaxInputTrim) * wetGain;
    return c;
  }

  // The bass cut follows the smoothed feedback once per block: flat up to where TIME reaches 75 %,
  // then sliding down to bassCutHzTop at the top of the knob.
  void refreshBassCut() {
    if (hpValid_ && hpFeedback_ == feedback_) return;
    using namespace chasm_detail;
    float hz = coefs_.bassHz;
    if (feedback_ > kBassSlideFrom) {
      const float slid = fminf((feedback_ - kBassSlideFrom) / (kBassSlideTo - kBassSlideFrom), 1.0f);
      hz *= powf(coefs_.bassHzTop / coefs_.bassHz, slid);
    }
    hp1_.a = hp2_.a = onePoleCoef(hz);
    hpFeedback_ = feedback_;
    hpValid_ = true;
  }

  // The input gain at a feedback g: kInputGain at the default, falling as (1 - g^2). A loop's energy
  // grows with the feedback, and this takes its rise out of the wet level.
  static float gainFor(float feedback) {
    using namespace chasm_detail;
    return kInputGain * ((1.0f - feedback * feedback) * kInvDefaultSpread);
  }

  void run(const float* in, float* wet, int n, const ChasmParams& p) {
    const float feedbackTarget = clampf(p.feedback, 0.0f, chasm_detail::kFeedbackMax);
    if (!valid_) {
      feedback_ = feedbackTarget;
      valid_ = true;
    }
    refresh(p.tuning);
    refreshBassCut();
    const Coefs& c = coefs_;

    // The wobble is evaluated at the block edges and the read heads move linearly between them. While
    // TIME is still the smoother holds its target, so the block runs at one feedback and gain. While
    // it moves, both are evaluated at the block edges too.
    const Wobble w = wobbleFor(n);
    const float inv = 1.0f / static_cast<float>(n);
    if (feedback_ == feedbackTarget) {
      const float feedback = feedback_;
      const float scale = c.inScale * gainFor(feedback);
      for (int i = 0; i < n; ++i) {
        const float f = static_cast<float>(i + 1) * inv;
        wet[i] = tick(clip(in[i]), feedback, scale, w.base1 + w.rise1 * f, w.base2 + w.rise2 * f);
      }
    } else {
      const float feedbackEnd = smooth::advance(feedback_, feedbackTarget, smooth::kSmoothCoef, n);
      const float fb0 = feedback_, fb1 = feedbackEnd;
      const float s0 = c.inScale * gainFor(fb0), s1 = c.inScale * gainFor(fb1);
      for (int i = 0; i < n; ++i) {
        const float f = static_cast<float>(i + 1) * inv;
        wet[i] = tick(clip(in[i]), fb0 + (fb1 - fb0) * f, s0 + (s1 - s0) * f, w.base1 + w.rise1 * f,
                      w.base2 + w.rise2 * f);
      }
      feedback_ = feedbackEnd;
    }
    c1_.runBlock(wet, n);
    c2_.runBlock(wet, n);
    c3_.runBlock(wet, n);
    c4_.runBlock(wet, n);
  }

  // Each read head's advance at the block's start and its change to the block's end. Sample i of n
  // reads at the fraction (i + 1) / n of the way, so the last sample lands on the end value. Advances
  // the LFO by the block.
  struct Wobble {
    float base1, base2, rise1, rise2;
  };

  Wobble wobbleFor(int n) {
    const Coefs& c = coefs_;
    const uint32_t end = lfoAcc_ + static_cast<uint32_t>(n) * c.lfoStep;
    const float a1 = reverb_parts::lfo(lfoAcc_, 0u) * c.depth, b1 = reverb_parts::lfo(end, 0u) * c.depth;
    const float a2 = reverb_parts::lfo(lfoAcc_, 64u) * c.depth, b2 = reverb_parts::lfo(end, 64u) * c.depth;
    lfoAcc_ = end;
    return {a1, a2, b1 - a1, b2 - a2};
  }

  float clip(float x) const { return softClipHeadroom(x * coefs_.drive, kReverbClipHeadroom); }

  // One sample of the loop for the clipped input x at the given input scale. Returns the sum of the
  // two filtered reads, which feeds the chirp stages.
  CV_INLINE float tick(float x, float feedback, float scale, float adv1, float adv2) {
    using chasm_detail::kLoopK;
    const float feed = x * scale;

    // Both reads come before either write, and the two lanes share no state, so the chains overlap.
    const float r1 = dly1_.read(adv1) * feedback;
    const float r2 = dly2_.read(adv2) * feedback;
    const float out1 = hp1_.hp(r1);
    const float out2 = hp2_.hp(r2);
    const float acc1 = a1c_.run(a1a_.run(out1, kLoopK), kLoopK);
    const float acc2 = a2c_.run(a2a_.run(out2, kLoopK), kLoopK);
    dly2_.write(acc1 + feed);
    dly1_.write(acc2 + feed);
    return out1 + out2;
  }

  chasm_detail::Allpass<chasm_detail::kAllp1A> a1a_;
  chasm_detail::Allpass<chasm_detail::kAllp1C> a1c_;
  chasm_detail::Allpass<chasm_detail::kAllp2A> a2a_;
  chasm_detail::Allpass<chasm_detail::kAllp2C> a2c_;
  chasm_detail::Delay<chasm_detail::kDly1> dly1_;
  chasm_detail::Delay<chasm_detail::kDly2> dly2_;
  chasm_detail::ChirpStage<3, 3, 0> c1_;
  chasm_detail::ChirpStage<5, 4, 1> c2_;
  chasm_detail::ChirpStage<6, 4, 2> c3_;
  chasm_detail::ChirpStage<7, 4, 3> c4_;
  OnePole hp1_, hp2_;      // each read's bass cut, set for hpFeedback_
  uint32_t lfoAcc_ = 0u;
  float feedback_ = 0.0f;  // the loop feedback, smoothed
  float hpFeedback_ = 0.0f;
  ChasmTuning tuning_;     // the tuning coefs_ was built from
  Coefs coefs_{};
  bool valid_ = false;
  bool built_ = false;
  bool hpValid_ = false;
};

}  // namespace cv
