#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/smooth.h"

// Distortion: behavioural Boss BD-2 model. Stage 1 (bass cut, top roll-off, gain,
// rail), tone stack, stage 2 (band limit, gain, rail), fixed post shaping, DC block.
// DRIVE sweeps both stage gains exponentially from gainNMin to gainNMax. The two rail saturators run 2x oversampled.

namespace cv {

struct DistortionTuning {
  float inputHpHz = 90.0f;        // input cap
  // stage 1 shaping, before its saturator
  float s1BassHz = 700.0f;        // low shelf cut corner
  float s1BassDb = -12.0f;        // low shelf depth below s1BassHz
  float s1LpHz = 6892.0f;         // top roll-off
  float gain1Min = 16.0f;         // stage gain at DRIVE 0
  float gain1Max = 45.0f;         // stage gain at DRIVE 1
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
  bool oversample = true;
};

struct DistortionParams {
  bool on = true;       // menu bypass
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
  }

  void process(const float* in, float* out, int n, const DistortionParams& p) {
    const DistortionTuning& t = p.tuning;
    const float target = p.on ? smooth::clamp01(p.drive) : 0.0f;

    // Settled drive 0 skips the chain. State clears once so re-entry starts clean.
    if (drive_ == 0.0f && target == 0.0f) {
      if (dirty_) clearChain();
      for (int i = 0; i < n; ++i) out[i] = in[i];
      return;
    }
    dirty_ = true;

    const float aDrive = smooth::coef(kDriveSec);
    const float toneTarget = smooth::clamp01(p.tone);
    setTrebleShelf(t);
    inHp_.set(t.inputHpHz);
    s1Lp_.set(t.s1LpHz);
    s2Hp_.set(t.s2HpHz);
    s2Lp_.set(t.s2LpHz);
    dc_.set(kDcBlockHz);
    s1Bass_.setLowShelf(t.s1BassHz, t.s1BassDb);
    stackBass_.setLowShelf(t.stackBassHz, t.stackBassDb);
    stackTreble_.setHighShelf(t.stackTrebleHz, t.stackTrebleDb);
    bassPeak_.setPeak(t.bassPeakHz, t.bassPeakDb, t.bassPeakQ);
    const Rail rail{1.0f - clampf(t.railAsym, 0.0f, 0.5f), clampf(t.railSoft, 0.01f, 1.0f)};
    const float stackLoss = dbToLin(t.stackLossDb);
    const float trim = dbToLin(t.trimDb);
    const float g1Min = t.gain1Min < kMinGain ? kMinGain : t.gain1Min;
    const float g2Min = t.gain2Min < kMinGain ? kMinGain : t.gain2Min;
    const float logR1 = logf((t.gain1Max < g1Min ? g1Min : t.gain1Max) / g1Min);
    const float logR2 = logf((t.gain2Max < g2Min ? g2Min : t.gain2Max) / g2Min);
    const float fadeSpan = t.fadeDrive < kMinFade ? kMinFade : t.fadeDrive;

    for (int i = 0; i < n; ++i) {
      drive_ = smooth::step(drive_, target, aDrive);
      const float prevTone = tone_;
      tone_ = smooth::step(tone_, toneTarget, aDrive);
      if (tone_ != prevTone) setTrebleShelf(t);
      const float g1 = g1Min * expf(logR1 * drive_);
      const float g2 = g2Min * expf(logR2 * drive_);

      float x = inHp_.hp(in[i]);
      x = s1Bass_.run(x);
      x = s1Lp_.lp(x);
      x = saturate(os1_, x * g1, rail, t.oversample);

      x = stackBass_.run(x);
      x = stackTreble_.run(x) * stackLoss;

      x = s2Hp_.hp(x);
      x = s2Lp_.lp(x);
      x = saturate(os2_, x * g2, rail, t.oversample);

      x = trebleCut_.run(x);
      x = bassPeak_.run(x);
      x = dc_.hp(x);
      // The fixed EQ fades in from exact passthrough over the first fadeDrive of DRIVE.
      const float wet = x * trim / makeup(g1 * g2);
      const float f = drive_ / fadeSpan;
      out[i] = f >= 1.0f ? wet : in[i] + f * (wet - in[i]);
    }
  }

 private:
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr float kDriveSec = 0.020f;
  static constexpr float kDcBlockHz = 10.0f;
  static constexpr float kMinFade = 1e-4f;
  static constexpr float kMinGain = 0.1f;
  static constexpr float kMinHz = 20.0f;
  static constexpr float kMaxHz = 0.45f * kSampleRate;
  static constexpr float kHalfPi = 1.57079632679489661923f;

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

  static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
  static float dbToLin(float db) { return powf(10.0f, db / 20.0f); }
  // TONE maps min..0 dB over the lower half and 0..max dB over the upper half, so
  // noon adds nothing.
  static float toneDb(const DistortionTuning& t, float tone) {
    return tone < 0.5f ? t.toneMinDb * (1.0f - 2.0f * tone) : t.toneMaxDb * (2.0f * tone - 1.0f);
  }
  void setTrebleShelf(const DistortionTuning& t) {
    trebleCut_.setHighShelf(t.trebleCutHz, t.trebleCutDb + toneDb(t, tone_));
  }

  // 1-pole; hp is the input minus its low-pass.
  struct Pole {
    float a = 0.0f, y = 0.0f;
    void set(float hz) { a = 1.0f - expf(-kTwoPi * clampf(hz, kMinHz, kMaxHz) / kSampleRate); }
    float lp(float x) {
      y += a * (x - y);
      return y;
    }
    float hp(float x) { return x - lp(x); }
  };

  // RBJ biquad, transposed direct form II. Zero dB gain is an exact identity.
  struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void setPeak(float hz, float db, float q) {
      const float w0 = kTwoPi * clampf(hz, kMinHz, kMaxHz) / kSampleRate;
      const float amp = powf(10.0f, db / 40.0f);
      const float cw = cosf(w0);
      const float alpha = sinf(w0) / (2.0f * (q < 0.1f ? 0.1f : q));
      const float a0 = 1.0f + alpha / amp;
      b0 = (1.0f + alpha * amp) / a0;
      b1 = -2.0f * cw / a0;
      b2 = (1.0f - alpha * amp) / a0;
      a1 = b1;
      a2 = (1.0f - alpha / amp) / a0;
    }

    // Shelf slope S = 1.
    void setLowShelf(float hz, float db) { setShelf(hz, db, -1.0f); }
    void setHighShelf(float hz, float db) { setShelf(hz, db, 1.0f); }

    // sign -1 low shelf, +1 high shelf.
    void setShelf(float hz, float db, float sign) {
      const float w0 = kTwoPi * clampf(hz, kMinHz, kMaxHz) / kSampleRate;
      const float amp = powf(10.0f, db / 40.0f);
      const float cw = cosf(w0);
      const float beta = 2.0f * sqrtf(amp) * (sinf(w0) * 0.70710678f);
      const float ap = amp + 1.0f, am = amp - 1.0f;
      const float a0 = ap - sign * am * cw + beta;
      b0 = amp * (ap + sign * am * cw + beta) / a0;
      b1 = -sign * 2.0f * amp * (am + sign * ap * cw) / a0;
      b2 = amp * (ap + sign * am * cw - beta) / a0;
      a1 = sign * 2.0f * (am - sign * ap * cw) / a0;
      a2 = (ap - sign * am * cw - beta) / a0;
    }

    float run(float x) {
      const float y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }

    void clear() { z1 = z2 = 0.0f; }
  };

  // Positive rail 1, negative rail -neg, tanh edge over the last soft fraction.
  struct Rail {
    float neg, soft;
    float operator()(float x) const {
      const float lim = x >= 0.0f ? 1.0f : neg;
      const float v = fabsf(x);
      const float edge = soft * lim;
      const float start = lim - edge;
      const float y = v <= start ? v : start + edge * tanhf((v - start) / edge);
      return x >= 0.0f ? y : -y;
    }
  };

  static void shiftIn(std::array<float, kHist>& h, float v) {
    for (int k = kHist - 1; k > 0; --k) h[k] = h[k - 1];
    h[0] = v;
  }

  static float halfBand(const std::array<float, kHist>& h) {
    return kH7 * (h[0] + h[7]) + kH5 * (h[1] + h[6]) + kH3 * (h[2] + h[5]) +
           kH1 * (h[3] + h[4]);
  }

  // Runs f at 2x through a half-band up and down pair.
  struct Oversampler {
    std::array<float, kHist> up{};
    std::array<float, kHist> downEven{};
    std::array<float, 4> downOdd{};

    template <class F>
    float run(float x, const F& f) {
      shiftIn(up, x);
      const float zEven = f(2.0f * halfBand(up));
      const float zOdd = f(up[3]);
      shiftIn(downEven, zEven);
      const float y = halfBand(downEven) + 0.5f * downOdd[3];
      for (int k = 3; k > 0; --k) downOdd[k] = downOdd[k - 1];
      downOdd[0] = zOdd;
      return y;
    }

    void clear() {
      up.fill(0.0f);
      downEven.fill(0.0f);
      downOdd.fill(0.0f);
    }
  };

  static float saturate(Oversampler& os, float x, const Rail& rail, bool oversample) {
    return oversample ? os.run(x, rail) : rail(x);
  }

  void clearChain() {
    for (Pole* p : {&inHp_, &s1Lp_, &s2Hp_, &s2Lp_, &dc_}) p->y = 0.0f;
    for (Biquad* b : {&s1Bass_, &stackBass_, &stackTreble_, &trebleCut_, &bassPeak_})
      b->clear();
    os1_.clear();
    os2_.clear();
    dirty_ = false;
  }

  Pole inHp_, s1Lp_, s2Hp_, s2Lp_, dc_;
  Biquad s1Bass_, stackBass_, stackTreble_, trebleCut_, bassPeak_;
  Oversampler os1_, os2_;
  float drive_ = 0.0f;
  float tone_ = 0.5f;
  bool dirty_ = false;
};

}  // namespace cv
