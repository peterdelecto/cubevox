#pragma once

#include <array>
#include <cmath>
#include <cstddef>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Distortion: behavioural Boss BD-2 model. Stage 1 (bass cut, top roll-off, gain,
// rail), tone stack, stage 2 (band limit, gain, rail), fixed post shaping, DC block.
// DRIVE sweeps both stage gains exponentially from gainNMin to gainNMax. Stage 1 gains
// topBoostDb more over the top of the knob, from topBoostFrom to DRIVE 1. The two
// rail saturators run 2x oversampled.

namespace cv {

struct DistortionTuning {
  float inputHpHz = 90.0f;        // input cap
  // stage 1 shaping, before its saturator
  float s1BassHz = 700.0f;        // low shelf cut corner
  float s1BassDb = -12.0f;        // low shelf depth below s1BassHz
  float s1LpHz = 6892.0f;         // top roll-off
  float gain1Min = 16.0f;         // stage gain at DRIVE 0
  float gain1Max = 45.0f;         // stage gain at DRIVE 1
  float topBoostDb = 6.0f;        // extra stage 1 gain at DRIVE 1, ramped in dB from topBoostFrom
  float topBoostFrom = 0.6f;      // DRIVE where the top boost starts
  // tone stack between the stages (bass 10 / mid 6 / treble 0)
  float stackBassHz = 400.0f;     // low shelf boost corner
  float stackBassDb = 10.0f;
  float stackTrebleHz = 2000.0f;  // high shelf cut corner
  float stackTrebleDb = -4.0f;
  float stackLossDb = -20.0f;     // insertion loss
  // stage 2
  float s2HpHz = 100.0f;
  float s2LpHz = 6645.0f;
  float gain2Min = 2.0f;
  float gain2Max = 7.5f;
  float railAsym = 0.05f;         // 0..0.5; positive rail 1, negative -(1 - asym)
  float railSoft = 0.1f;          // soft edge as a fraction of the rail
  // post
  float trebleCutHz = 1000.0f;    // fixed high shelf cut
  float trebleCutDb = -5.4f;
  float toneMinDb = -12.0f;       // TONE fully down: extra shelf gain above trebleCutHz
  float toneMaxDb = 6.0f;         // TONE fully up
  float bassPeakHz = 120.0f;      // gyrator bump
  float bassPeakDb = 2.0f;
  float bassPeakQ = 1.0f;
  float trimDb = 3.3f;            // on top of the makeup law; level rule
  float fadeDrive = 0.05f;        // dry-to-chain crossfade span from DRIVE 0
};

struct DistortionParams {
  bool on = true;       // panel toggle
  float drive = 0.0f;   // panel knob 0..1
  float tone = 0.5f;    // panel knob 0..1; noon is flat
  DistortionTuning tuning;
};

class Distortion {
 public:
  void reset() {
    clearChain();
    drive_ = 0.0f;
    tone_ = 0.5f;
    tuningValid_ = false;
    shelfTone_ = -1.0f;
  }

  void process(const float* CV_RESTRICT in, float* CV_RESTRICT out, int n,
               const DistortionParams& p) {
    const DistortionTuning& t = p.tuning;
    const float target = p.on ? smooth::clamp01(p.drive) : 0.0f;

    // Settled drive 0 skips the chain. State clears once so re-entry starts clean.
    if (drive_ == 0.0f && target == 0.0f) {
      if (dirty_) clearChain();
      for (int i = 0; i < n; ++i) out[i] = in[i];
      return;
    }
    dirty_ = true;

    if (!tuningValid_ || !same(t, applied_)) applyTuning(t);
    if (tone_ != shelfTone_) {
      setTrebleShelf(t);
      shelfTone_ = tone_;
    }
    runBlock(in, out, n, target, smooth::clamp01(p.tone));
  }

  // Stage 1 gain at DRIVE d for the tuning the last process() call applied.
  float stage1GainAt(float d) const { return gainsAt(d).g1; }

 private:
  static constexpr float kDcBlockHz = 10.0f;
  static constexpr float kMinFade = 1e-4f;
  static constexpr float kMinGain = 0.1f;
  static constexpr float kLn10 = 2.302585093f;

  // Stage gains and the output scale at one DRIVE value.
  struct Gains {
    float g1, g2, k;
  };

  // The top boost ramps in dB, so it adds to the stage 1 exponent. Below topBoostFrom the
  // added term is exactly 0.0f and the gain equals the plain exponential sweep bit for bit.
  Gains gainsAt(float drive) const {
    const float over = drive > boostFrom_ ? drive - boostFrom_ : 0.0f;
    const float g1 = g1Min_ * expf(logR1_ * drive + boostK_ * over);
    const float g2 = g2Min_ * expf(logR2_ * drive);
    return {g1, g2, trim_ / makeup(g1 * g2)};
  }

  // Stage gains and makeup are evaluated at the block edges and interpolated. The loop runs on a
  // local copy of the chain so stores to out cannot alias filter state.
  void runBlock(const float* CV_RESTRICT in, float* CV_RESTRICT out, int n, float target,
                float toneTarget) {
    float driveEnd = drive_;
    for (int i = 0; i < n; ++i) driveEnd = smooth::step(driveEnd, target, smooth::kSmoothCoef);
    const Gains a = gainsAt(drive_);
    const Gains b = gainsAt(driveEnd);
    const float inv = n > 0 ? 1.0f / static_cast<float>(n) : 0.0f;
    Chain c = chain_;
    const Rail rail = rail_;
    const float stackLoss = stackLoss_, fadeSpan = fadeSpan_;
    float drive = drive_, tone = tone_;
    for (int i = 0; i < n; ++i) {
      drive = smooth::step(drive, target, smooth::kSmoothCoef);
      tone = smooth::step(tone, toneTarget, smooth::kSmoothCoef);
      const float fi = static_cast<float>(i + 1) * inv;
      const float g1 = a.g1 + (b.g1 - a.g1) * fi;
      const float g2 = a.g2 + (b.g2 - a.g2) * fi;
      const float wet = c.run(in[i], g1, g2, rail, stackLoss) * (a.k + (b.k - a.k) * fi);
      // The fixed EQ fades in from exact passthrough over the first fadeDrive of DRIVE.
      const float f = drive / fadeSpan;
      out[i] = f >= 1.0f ? wet : in[i] + f * (wet - in[i]);
    }
    chain_ = c;
    drive_ = drive;
    tone_ = tone;
  }

  // makeup(g) = a*g / sqrt(1 + (a*g / s)^2): follows the linear stage gain (a = 0.06)
  // and flattens at the saturated level (s = 10.3) so a -18 dBFS RMS vocal holds level.
  static float makeup(float gain) {
    const float lin = kMakeupLinear * gain;
    const float r = lin / kMakeupSat;
    return lin / sqrtf(1.0f + r * r);
  }
  static constexpr float kMakeupLinear = 0.06f;
  static constexpr float kMakeupSat = 10.3f;

  // Half-band FIR, 15 taps, Hamming window. Taps at even offsets are zero and the
  // centre is 0.5, so each phase needs only the few taps below.
  static constexpr float kH1 = 0.30381f;
  static constexpr float kH3 = -0.06815f;
  static constexpr float kH5 = 0.016118f;
  static constexpr float kH7 = -0.003638f;
  static constexpr int kHist = 8;

  static float dbToLin(float db) { return powf(10.0f, db / 20.0f); }
  // TONE maps min..0 dB over the lower half and 0..max dB over the upper half, so
  // noon adds nothing.
  static float toneDb(const DistortionTuning& t, float tone) {
    return tone < 0.5f ? t.toneMinDb * (1.0f - 2.0f * tone) : t.toneMaxDb * (2.0f * tone - 1.0f);
  }
  static bool same(const DistortionTuning& a, const DistortionTuning& b) {
    return a.inputHpHz == b.inputHpHz && a.s1BassHz == b.s1BassHz && a.s1BassDb == b.s1BassDb &&
           a.s1LpHz == b.s1LpHz && a.gain1Min == b.gain1Min && a.gain1Max == b.gain1Max &&
           a.topBoostDb == b.topBoostDb && a.topBoostFrom == b.topBoostFrom &&
           a.stackBassHz == b.stackBassHz && a.stackBassDb == b.stackBassDb &&
           a.stackTrebleHz == b.stackTrebleHz && a.stackTrebleDb == b.stackTrebleDb &&
           a.stackLossDb == b.stackLossDb && a.s2HpHz == b.s2HpHz && a.s2LpHz == b.s2LpHz &&
           a.gain2Min == b.gain2Min && a.gain2Max == b.gain2Max && a.railAsym == b.railAsym &&
           a.railSoft == b.railSoft && a.trebleCutHz == b.trebleCutHz &&
           a.trebleCutDb == b.trebleCutDb && a.toneMinDb == b.toneMinDb &&
           a.toneMaxDb == b.toneMaxDb && a.bassPeakHz == b.bassPeakHz &&
           a.bassPeakDb == b.bassPeakDb && a.bassPeakQ == b.bassPeakQ && a.trimDb == b.trimDb &&
           a.fadeDrive == b.fadeDrive;
  }

  // Recomputes everything that depends on tuning alone.
  void applyTuning(const DistortionTuning& t) {
    chain_.inHp.set(t.inputHpHz);
    chain_.s1Lp.set(t.s1LpHz);
    chain_.s2Hp.set(t.s2HpHz);
    chain_.s2Lp.set(t.s2LpHz);
    chain_.dc.set(kDcBlockHz);
    chain_.s1Bass.setLowShelf(t.s1BassHz, t.s1BassDb);
    chain_.stackBass.setLowShelf(t.stackBassHz, t.stackBassDb);
    chain_.stackTreble.setHighShelf(t.stackTrebleHz, t.stackTrebleDb);
    chain_.bassPeak.setPeak(t.bassPeakHz, t.bassPeakDb, t.bassPeakQ);
    rail_ = Rail{1.0f - clampf(t.railAsym, 0.0f, 0.5f), clampf(t.railSoft, 0.01f, 1.0f)};
    stackLoss_ = dbToLin(t.stackLossDb);
    trim_ = dbToLin(t.trimDb);
    g1Min_ = t.gain1Min < kMinGain ? kMinGain : t.gain1Min;
    g2Min_ = t.gain2Min < kMinGain ? kMinGain : t.gain2Min;
    logR1_ = logf((t.gain1Max < g1Min_ ? g1Min_ : t.gain1Max) / g1Min_);
    logR2_ = logf((t.gain2Max < g2Min_ ? g2Min_ : t.gain2Max) / g2Min_);
    boostFrom_ = clampf(t.topBoostFrom, 0.0f, 1.0f);
    const float boostRoom = 1.0f - boostFrom_;
    const float boostSpan = boostRoom < kMinFade ? kMinFade : boostRoom;
    boostK_ = t.topBoostDb * (kLn10 / 20.0f) / boostSpan;
    fadeSpan_ = t.fadeDrive < kMinFade ? kMinFade : t.fadeDrive;
    applied_ = t;
    tuningValid_ = true;
    shelfTone_ = -1.0f;
  }

  void setTrebleShelf(const DistortionTuning& t) {
    chain_.trebleCut.setHighShelf(t.trebleCutHz, t.trebleCutDb + toneDb(t, tone_));
  }

  // Positive rail 1, negative rail -neg, tanh edge over the last soft fraction.
  struct Rail {
    float neg, soft;
    CV_INLINE float operator()(float x) const {
      const float lim = x >= 0.0f ? 1.0f : neg;
      const float v = fabsf(x);
      const float edge = soft * lim;
      const float start = lim - edge;
      const float y = v <= start ? v : start + edge * tanhFast((v - start) / edge);
      return x >= 0.0f ? y : -y;
    }
  };

  // Ring history of N samples; at(0) is the newest.
  template <std::size_t N>
  struct Ring {
    static_assert((N & (N - 1)) == 0, "ring length must be a power of two");
    static constexpr std::size_t kMask = N - 1;

    std::array<float, N> buf{};
    std::size_t newest = 0;

    CV_INLINE void push(float v) {
      newest = (newest - 1) & kMask;
      buf[newest] = v;
    }
    CV_INLINE float at(std::size_t k) const { return buf[(newest + k) & kMask]; }
    void clear() {
      buf.fill(0.0f);
      newest = 0;
    }
  };

  static CV_INLINE float halfBand(const Ring<kHist>& h) {
    static_assert(kHist == 8, "halfBand taps assume 8 samples");
    return kH7 * (h.at(0) + h.at(7)) + kH5 * (h.at(1) + h.at(6)) +
           kH3 * (h.at(2) + h.at(5)) + kH1 * (h.at(3) + h.at(4));
  }

  // Runs f at 2x through a half-band up and down pair.
  struct Oversampler {
    Ring<kHist> up;
    Ring<kHist> downEven;
    Ring<4> downOdd;

    template <class F>
    CV_INLINE float run(float x, const F& f) {
      up.push(x);
      const float zEven = f(2.0f * halfBand(up));
      const float zOdd = f(up.at(3));
      downEven.push(zEven);
      const float y = halfBand(downEven) + 0.5f * downOdd.at(3);
      downOdd.push(zOdd);
      return y;
    }

    void clear() {
      up.clear();
      downEven.clear();
      downOdd.clear();
    }
  };

  // The signal path, in order. Stage gains g1 and g2 come from DRIVE.
  struct Chain {
    OnePole inHp, s1Lp, s2Hp, s2Lp, dc;
    Biquad s1Bass, stackBass, stackTreble, trebleCut, bassPeak;
    Oversampler os1, os2;

    CV_INLINE float run(float x, float g1, float g2, const Rail& rail, float stackLoss) {
      x = s1Lp.lp(s1Bass.run(inHp.hp(x)));
      x = os1.run(x * g1, rail);
      x = stackTreble.run(stackBass.run(x)) * stackLoss;
      x = s2Lp.lp(s2Hp.hp(x));
      x = os2.run(x * g2, rail);
      return dc.hp(bassPeak.run(trebleCut.run(x)));
    }

    void clear() {
      for (OnePole* p : {&inHp, &s1Lp, &s2Hp, &s2Lp, &dc}) p->clear();
      for (Biquad* b : {&s1Bass, &stackBass, &stackTreble, &trebleCut, &bassPeak}) b->clear();
      os1.clear();
      os2.clear();
    }
  };

  void clearChain() {
    chain_.clear();
    dirty_ = false;
  }

  Chain chain_;
  float drive_ = 0.0f;
  float tone_ = 0.5f;
  bool dirty_ = false;

  DistortionTuning applied_;
  bool tuningValid_ = false;
  float shelfTone_ = -1.0f;
  Rail rail_{1.0f, 1.0f};
  float stackLoss_ = 1.0f, trim_ = 1.0f, g1Min_ = 1.0f, g2Min_ = 1.0f;
  float logR1_ = 0.0f, logR2_ = 0.0f, fadeSpan_ = 1.0f;
  float boostFrom_ = 1.0f, boostK_ = 0.0f;  // top boost: start DRIVE, exponent per unit DRIVE past it
};

}  // namespace cv
