#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Lo-fi: the voice through a cheap speaker. TONE picks the speaker (a dark radio, a telephone, a thin
// megaphone) by moving the centre of a band on a log scale. AMOUNT narrows the band and drives it over
// its first half, then adds a sample-and-hold with no anti-alias filter and a bit crush over its second
// half. Signal path: 4th-order high-pass, 4th-order low-pass, a peaking honk at the centre, tanh drive,
// the crush, then the inverse of the drive gain times the makeup gain.
// The crush runs ahead of the inverse, where the drive output fills the range, so 5 bits means 5 bits.
// After the inverse the voice peaks near 1/8 of full scale and a 5-bit step leaves it five levels.
// Filter coefficients, the crush law and the gains are set once per block from the 20 ms knob
// smoothers; the sample loop runs no libm. The two gains and the five filters' coefficients ramp
// linearly across the block, so a moving knob never hands a section a coefficient step.

namespace cv {

constexpr size_t kLofiMakeupTones = 5;      // rows: TONE 0, 25, 50, 75, 100 %
constexpr size_t kLofiMakeupAmounts = 11;   // columns: AMOUNT 0, 10, ... 100 %

struct LofiTuning {
  float fcLoHz = 600.0f;       // band centre at TONE 0
  float fcHiHz = 2400.0f;      // band centre at TONE 1
  float hpStartHz = 60.0f;     // high-pass corner at AMOUNT 0
  float hpEndRatio = 0.5f;     // corner over the centre from AMOUNT 50 % up
  float lpStartHz = 14000.0f;  // low-pass corner at AMOUNT 0
  float lpEndRatio = 2.0f;     // corner over the centre from AMOUNT 50 % up
  float honkDb = 6.0f;         // peak gain at the centre from AMOUNT 50 % up
  float honkQ = 1.4f;
  float driveDb = 18.0f;       // gain ahead of the tanh from AMOUNT 50 % up
  float holdMaxHz = 48000.0f;  // held rate at AMOUNT 50 %
  float holdMinHz = 6000.0f;   // held rate at AMOUNT 100 %
  float bitsMax = 16.0f;       // bit depth at AMOUNT 50 %
  float bitsMin = 5.0f;        // bit depth at AMOUNT 100 %

  // Gain in dB that brings program RMS back to its input level, one row per TONE point and one column
  // per AMOUNT point. Each cell is the mean loss over Adam's three clips, interpolated bilinearly in dB.
  // The loss steepens up to AMOUNT 50 %, where the band reaches its full narrowness, and stays flat above.
  std::array<std::array<float, kLofiMakeupAmounts>, kLofiMakeupTones> makeupDb = {{
      {{0.27f, 0.31f, 0.88f, 1.37f, 2.37f, 3.62f, 3.62f, 3.62f, 3.62f, 3.62f, 3.61f}},
      {{0.27f, 0.60f, 1.42f, 2.29f, 3.46f, 5.41f, 5.41f, 5.41f, 5.41f, 5.41f, 5.39f}},
      {{0.27f, 0.87f, 1.82f, 3.11f, 4.61f, 8.02f, 8.02f, 8.02f, 8.02f, 8.02f, 8.00f}},
      {{0.27f, 1.11f, 2.20f, 3.73f, 6.46f, 10.60f, 10.60f, 10.60f, 10.60f, 10.60f, 10.57f}},
      {{0.27f, 1.30f, 2.60f, 4.41f, 8.87f, 13.28f, 13.28f, 13.27f, 13.28f, 13.29f, 13.22f}},
  }};

  // On top of the makeup law. It puts the default level of two of Adam's three clips in 0 to +0.5 dB
  // (+0.10, +0.37 and -0.77 dB).
  float trimDb = -0.1f;
};

struct LofiParams {
  bool on = true;       // panel toggle
  float tone = 0.5f;    // upper pot 0..1
  float amount = 0.5f;  // lower pot 0..1
  LofiTuning tuning;
};

class Lofi {
 public:
  // Everything the knobs set, for one TONE and AMOUNT.
  struct Law {
    float fcHz, hpHz, lpHz, honkDb;
    float drive;       // gain ahead of the tanh
    float makeup;      // linear makeup; the output gain after the crush is makeup / drive
    float holdRatio;   // held rate over the sample rate, 1 holds nothing
    float step;        // crush step q = 2^(1 - bits)
    float bits;
    bool crush;        // AMOUNT above 50 %
  };

  static Law lawAt(float tone, float amount, const LofiTuning& t) {
    tone = smooth::clamp01(tone);
    amount = smooth::clamp01(amount);
    const float a = amount >= 0.5f ? 1.0f : 2.0f * amount;
    const float c = amount > 0.5f ? 2.0f * amount - 1.0f : 0.0f;
    Law law;
    law.fcHz = t.fcLoHz * powf(t.fcHiHz / t.fcLoHz, tone);
    law.hpHz = t.hpStartHz * powf(t.hpEndRatio * law.fcHz / t.hpStartHz, a);
    law.lpHz = t.lpStartHz * powf(t.lpEndRatio * law.fcHz / t.lpStartHz, a);
    law.honkDb = t.honkDb * a;
    law.drive = dbToGainFast(t.driveDb * a);
    law.makeup = dbToGainFast(makeupDbAt(tone, amount, t) + t.trimDb);
    const float holdHz = t.holdMaxHz * powf(t.holdMinHz / t.holdMaxHz, c);
    law.holdRatio = clampf(holdHz / kSampleRate, kMinHoldRatio, 1.0f);
    law.bits = clampf(t.bitsMax + (t.bitsMin - t.bitsMax) * c, kMinBits, kMaxBits);
    law.step = exp2f(1.0f - law.bits);
    law.crush = c > 0.0f;
    return law;
  }

  void reset() {
    clearVoice();
    onMix_ = 0.0f;
    tone_ = 0.5f;
    amount_ = 0.5f;
    lawValid_ = false;
  }

  void process(const float* in, float* out, int n, const LofiParams& p) {
    for (int done = 0; done < n; done += kBlock) {
      const int m = n - done < kBlock ? n - done : kBlock;
      runBlock(in + done, out + done, m, p);
    }
  }

 private:
  static constexpr float kMinHoldRatio = 0.01f;
  static constexpr float kMinBits = 1.0f;
  static constexpr float kMaxBits = 20.0f;

  // The two sections of a 4th-order Butterworth.
  static constexpr float kQ1 = 0.54f;
  static constexpr float kQ2 = 1.31f;

  static inline const float kBlockSmoothCoef = smooth::blockCoef(smooth::kSmoothSec, kBlock);

  // Index and fraction of x in 0..1 on a grid of `points` evenly spaced points.
  static void gridPos(float x, size_t points, size_t* i, float* frac) {
    const float pos = x * static_cast<float>(points - 1);
    size_t k = static_cast<size_t>(pos);
    if (k > points - 2) k = points - 2;
    *i = k;
    *frac = pos - static_cast<float>(k);
  }

  // The table in dB at TONE and AMOUNT, linear between its points along both axes.
  static float makeupDbAt(float tone, float amount, const LofiTuning& t) {
    size_t ti, ai;
    float tf, af;
    gridPos(tone, kLofiMakeupTones, &ti, &tf);
    gridPos(amount, kLofiMakeupAmounts, &ai, &af);
    const auto row = [&](size_t r) { return t.makeupDb[r][ai] + af * (t.makeupDb[r][ai + 1] - t.makeupDb[r][ai]); };
    const float lo = row(ti);
    return lo + tf * (row(ti + 1) - lo);
  }

  // Gains the sample loop ramps between. post is the inverse of the drive times the makeup.
  struct Gains {
    float drive, post;
  };

  void clearVoice() {
    hp1_.clear();
    hp2_.clear();
    lp1_.clear();
    lp2_.clear();
    honk_.clear();
    held_ = 0.0f;
    phase_ = 1.0f;
  }

  // A new block of the stage. A settled-off stage is a plain copy. The voice clears on the enable
  // edge only when the dry/wet mix sits at exactly 0, with the knobs snapped to their targets.
  void runBlock(const float* in, float* out, int n, const LofiParams& p) {
    const float target = p.on ? 1.0f : 0.0f;
    if (onMix_ == 0.0f) {
      if (target == 0.0f) {
        for (int i = 0; i < n; ++i) out[i] = in[i];
        return;
      }
      clearVoice();
      tone_ = smooth::clamp01(p.tone);
      amount_ = smooth::clamp01(p.amount);
      lawValid_ = false;
    }

    const float coef = n == kBlock ? kBlockSmoothCoef : smooth::blockCoef(smooth::kSmoothSec, n);
    tone_ = smooth::step(tone_, smooth::clamp01(p.tone), coef);
    amount_ = smooth::step(amount_, smooth::clamp01(p.amount), coef);
    if (!lawValid_ || tone_ != lawTone_ || amount_ != lawAmount_ ||
        std::memcmp(&p.tuning, &applied_, sizeof applied_) != 0)
      applyLaw(p.tuning);
    loop(in, out, n, target);
  }

  // Sets the five filters' end-of-block coefficients and the block's end gains from the smoothed
  // knobs. The first law after a reset takes them at once; a later one is reached by the block's ramp.
  void applyLaw(const LofiTuning& t) {
    law_ = lawAt(tone_, amount_, t);
    goal_[0].setHighPass(law_.hpHz, kQ1);
    goal_[1].setHighPass(law_.hpHz, kQ2);
    goal_[2].setLowPass(law_.lpHz, kQ1);
    goal_[3].setLowPass(law_.lpHz, kQ2);
    goal_[4].setPeak(law_.fcHz, law_.honkDb, t.honkQ);
    next_ = {law_.drive, law_.makeup / law_.drive};
    if (!lawValid_) {
      cur_ = next_;
      hp1_.setCoefsFrom(goal_[0]);
      hp2_.setCoefsFrom(goal_[1]);
      lp1_.setCoefsFrom(goal_[2]);
      lp2_.setCoefsFrom(goal_[3]);
      honk_.setCoefsFrom(goal_[4]);
    }
    invQ_ = 1.0f / law_.step;
    lawTone_ = tone_;
    lawAmount_ = amount_;
    applied_ = t;
    lawValid_ = true;
  }

  // The per-sample steps that take one section's coefficients to its goal over a block.
  struct CoefStep {
    float b0, b1, b2, a1, a2;
  };

  static CoefStep stepsTo(const Biquad& from, const Biquad& to, float inv) {
    return {(to.b0 - from.b0) * inv, (to.b1 - from.b1) * inv, (to.b2 - from.b2) * inv, (to.a1 - from.a1) * inv,
            (to.a2 - from.a2) * inv};
  }

  static void advance(Biquad& b, const CoefStep& s) {
    b.b0 += s.b0;
    b.b1 += s.b1;
    b.b2 += s.b2;
    b.a1 += s.a1;
    b.a2 += s.a2;
  }

  static bool apart(const Biquad& a, const Biquad& b) {
    return a.b0 != b.b0 || a.b1 != b.b1 || a.b2 != b.b2 || a.a1 != b.a1 || a.a2 != b.a2;
  }

  // A block whose filters sit on their goals runs the plain loop. One that has a goal to reach ramps
  // every coefficient to it, so the sections never see a step.
  void loop(const float* in, float* out, int n, float target) {
    if (apart(hp1_, goal_[0]) || apart(hp2_, goal_[1]) || apart(lp1_, goal_[2]) || apart(lp2_, goal_[3]) ||
        apart(honk_, goal_[4])) {
      run<true>(in, out, n, target);
    } else {
      run<false>(in, out, n, target);
    }
  }

  // Band, honk, drive, crush, makeup, then the dry/wet mix. The filters and the gains run on local
  // copies, so stores to out cannot alias them.
  template <bool kRamp>
  void run(const float* in, float* out, int n, float target) {
    Biquad hp1 = hp1_, hp2 = hp2_, lp1 = lp1_, lp2 = lp2_, honk = honk_;
    const float inv = 1.0f / static_cast<float>(n);
    CoefStep s0{}, s1{}, s2{}, s3{}, s4{};
    if constexpr (kRamp) {
      s0 = stepsTo(hp1, goal_[0], inv);
      s1 = stepsTo(hp2, goal_[1], inv);
      s2 = stepsTo(lp1, goal_[2], inv);
      s3 = stepsTo(lp2, goal_[3], inv);
      s4 = stepsTo(honk, goal_[4], inv);
    }
    float g = cur_.drive, post = cur_.post;
    const float dg = (next_.drive - g) * inv;
    const float dpost = (next_.post - post) * inv;
    const bool crush = law_.crush;
    const float ratio = law_.holdRatio, step = law_.step, invStep = invQ_;
    float phase = phase_, held = held_, mix = onMix_;
    for (int i = 0; i < n; ++i) {
      const float x = in[i];
      if constexpr (kRamp) {
        advance(hp1, s0);
        advance(hp2, s1);
        advance(lp1, s2);
        advance(lp2, s3);
        advance(honk, s4);
      }
      float v = honk.run(lp2.run(lp1.run(hp2.run(hp1.run(x)))));
      v = tanhFast(v * g);
      if (crush) {
        phase += ratio;
        if (phase >= 1.0f) {
          phase -= 1.0f;
          const float s = v * invStep;
          held = step * static_cast<float>(static_cast<int>(s + (s >= 0.0f ? 0.5f : -0.5f)));
        }
        v = held;
      }
      v *= post;
      mix = smooth::step(mix, target, smooth::kSmoothCoef);
      out[i] = mix >= 1.0f ? v : x + mix * (v - x);
      g += dg;
      post += dpost;
    }
    if constexpr (kRamp) {  // land on the goals exactly, whatever the steps added up to
      hp1.setCoefsFrom(goal_[0]);
      hp2.setCoefsFrom(goal_[1]);
      lp1.setCoefsFrom(goal_[2]);
      lp2.setCoefsFrom(goal_[3]);
      honk.setCoefsFrom(goal_[4]);
    }
    hp1_ = hp1;
    hp2_ = hp2;
    lp1_ = lp1;
    lp2_ = lp2;
    honk_ = honk;
    cur_ = next_;
    onMix_ = mix;

    // Without the crush the next block that needs it takes a fresh sample on its first sample.
    phase_ = crush ? phase : 1.0f;
    held_ = crush ? held : 0.0f;
  }

  Biquad hp1_, hp2_, lp1_, lp2_, honk_;
  std::array<Biquad, 5> goal_;  // the coefficients the block ends on; the states are unused
  Law law_{};
  Gains cur_{1.0f, 1.0f};
  Gains next_{1.0f, 1.0f};
  LofiTuning applied_;
  float invQ_ = 1.0f;
  float tone_ = 0.5f;
  float amount_ = 0.5f;
  float lawTone_ = 0.0f;
  float lawAmount_ = 0.0f;
  float onMix_ = 0.0f;
  float phase_ = 1.0f;
  float held_ = 0.0f;
  bool lawValid_ = false;
};

}  // namespace cv
