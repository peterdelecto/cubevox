#pragma once

#include <array>
#include <cmath>
#include <cstddef>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"
#include "engine/tanh_cubic.h"

// Lo-fi: the voice through a cheap speaker. TONE picks the speaker (a dark radio, a telephone, a thin
// megaphone) by moving the centre of a band on a log scale. AMOUNT narrows the band and drives it over
// its first 83 %, then adds a sample-and-hold with no anti-alias filter and a bit crush over the last
// 17 %. Signal path: 4th-order high-pass, 4th-order low-pass, a peaking honk at the centre, tanh drive,
// the crush, then the inverse of the drive gain times the makeup gain.
// The crush runs ahead of the inverse, where the drive output fills the range, so the bit depth is
// a fraction of the drive's full scale.
// Filter coefficients, the crush law and the gains are set once per block from the 20 ms knob
// smoothers; the sample loop runs no libm. The two gains, the held rate and the five filters'
// coefficients ramp linearly across the block, so a moving knob never hands a section a step.
// A block runs in stages over a scratch of its own length. The filters run first, a block-long loop per
// section while the coefficients ramp and one loop for all five once they sit still. The drive and
// tanh follow, then the crush, the makeup and the mix. Each loop keeps its values in registers.

namespace cv {

constexpr size_t kLofiMakeupTones = 5;     // rows: TONE 0, 25, 50, 75, 100 %
constexpr size_t kLofiMakeupAmounts = 7;   // columns: the law's AMOUNT 0, 10, ... 60 %

// The law's AMOUNT runs 0 to 1, and the pot covers the first part of it. At the pot's top the band is
// closed and driven and the crush has only begun: a 31.7 kHz hold at 13.8 bits.
constexpr float kLofiAmountTop = 0.6f;

// Makeup columns per unit of the law's AMOUNT.
constexpr float kLofiColumnsPerAmount = 10.0f;

struct LofiTuning {
  float fcLoHz = 600.0f;       // band centre at TONE 0
  float fcHiHz = 2400.0f;      // band centre at TONE 1
  float hpStartHz = 60.0f;     // high-pass corner at AMOUNT 0
  float hpEndRatio = 0.5f;     // corner over the centre from the law's AMOUNT 50 % up
  float lpStartHz = 14000.0f;  // low-pass corner at AMOUNT 0
  float lpEndRatio = 2.0f;     // corner over the centre from the law's AMOUNT 50 % up
  float honkDb = 6.0f;         // peak gain at the centre from the law's AMOUNT 50 % up
  float honkQ = 1.4f;
  float driveDb = 18.0f;       // gain ahead of the tanh from the law's AMOUNT 50 % up
  float holdMaxHz = static_cast<float>(kSampleRate);  // held rate at the law's AMOUNT 50 %
  float holdMinHz = 6000.0f;   // held rate at the law's AMOUNT 100 %
  float bitsMax = 16.0f;       // bit depth at the law's AMOUNT 50 %
  float bitsMin = 5.0f;        // bit depth at the law's AMOUNT 100 %

  // Gain in dB that brings program RMS back to its input level, one row per TONE point and one column
  // per 10 % of the law's AMOUNT. Each cell is the mean loss over Adam's three clips, interpolated
  // bilinearly in dB. The loss steepens up to AMOUNT 50 %, where the band reaches its full narrowness,
  // and stays flat above.
  std::array<std::array<float, kLofiMakeupAmounts>, kLofiMakeupTones> makeupDb = {{
      {{0.16f, 0.19f, 0.76f, 1.25f, 2.25f, 3.50f, 3.50f}},
      {{0.16f, 0.48f, 1.30f, 2.17f, 3.34f, 5.29f, 5.29f}},
      {{0.16f, 0.75f, 1.71f, 2.99f, 4.50f, 7.90f, 7.90f}},
      {{0.16f, 0.99f, 2.08f, 3.62f, 6.34f, 10.48f, 10.48f}},
      {{0.16f, 1.18f, 2.49f, 4.29f, 8.75f, 13.16f, 13.16f}},
  }};

  // On top of the makeup law. It puts the box default on Adam's three clips at +0.08 to +0.19 dB.
  float trimDb = 0.15f;
};

struct LofiParams {
  bool on = true;       // panel toggle
  float tone = 0.5f;    // upper pot 0..1
  float amount = 0.5f;  // lower pot 0..1
  LofiTuning tuning;
};

class Lofi {
 public:
  // The limits the law clamps to, for a caller that checks a tuning.
  static constexpr float kMinHoldRatio = 0.01f;
  static constexpr float kMinBits = 1.0f;
  static constexpr float kMaxBits = 20.0f;

  // Everything the knobs set, for one TONE and AMOUNT.
  struct Law {
    float fcHz, hpHz, lpHz, honkDb;
    float drive;       // gain ahead of the tanh
    float makeup;      // linear makeup; the output gain after the crush is makeup / drive
    float holdRatio;   // held rate over the sample rate, 1 holds nothing
    float step;        // crush step q = 2^(1 - bits); 1 while the crush is off
    float bits;
    bool crush;        // the law's AMOUNT is above 50 %
  };

  // The law at TONE and at the AMOUNT pot position, both 0..1.
  static Law lawAt(float tone, float pot, const LofiTuning& t) {
    return lawOf(smooth::clamp01(tone), smooth::clamp01(pot) * kLofiAmountTop, t, logsOf(t));
  }

  void reset() {
    clearVoice();
    onMix_ = 0.0f;
  }

  void process(const float* in, float* out, int n, const LofiParams& p) {
    for (int done = 0; done < n; done += kBlock) {
      const int m = n - done < kBlock ? n - done : kBlock;
      runBlock(in + done, out + done, m, p);
    }
  }

 private:
  // The two sections of a 4th-order Butterworth.
  static constexpr float kQ1 = 0.54f;
  static constexpr float kQ2 = 1.31f;

  static constexpr float kBlockSmoothCoef = smooth::kSmoothBlockCoef;

  // log2 of the tuning's corner frequencies, so a corner is one exp2Fast. They change with the tuning only.
  struct Logs {
    float fcLo, fcSpan, hpStart, hpEnd, lpStart, lpEnd;
  };

  static Logs logsOf(const LofiTuning& t) {
    return {log2f(t.fcLoHz),    log2f(t.fcHiHz / t.fcLoHz), log2f(t.hpStartHz),
            log2f(t.hpEndRatio), log2f(t.lpStartHz),        log2f(t.lpEndRatio)};
  }

  // `amount` is the law's AMOUNT. The band and the drive travel over its first half and the crush over
  // its second. The held rate and the step keep libm, because they set where every held sample lands
  // and a one-ulp rate drifts the hold's phase onto other input samples.
  static Law lawOf(float tone, float amount, const LofiTuning& t, const Logs& l) {
    const float a = amount >= 0.5f ? 1.0f : 2.0f * amount;
    const float c = amount > 0.5f ? 2.0f * amount - 1.0f : 0.0f;
    const float logFc = l.fcLo + tone * l.fcSpan;
    Law law;
    law.fcHz = exp2Fast(logFc);
    law.hpHz = exp2Fast(l.hpStart + a * (l.hpEnd + logFc - l.hpStart));
    law.lpHz = exp2Fast(l.lpStart + a * (l.lpEnd + logFc - l.lpStart));
    law.honkDb = t.honkDb * a;
    law.drive = dbToGainFast(t.driveDb * a);
    law.makeup = dbToGainFast(makeupDbAt(tone, amount, t) + t.trimDb);
    law.crush = c > 0.0f;
    law.bits = clampf(t.bitsMax + (t.bitsMin - t.bitsMax) * c, kMinBits, kMaxBits);
    law.step = law.crush ? exp2f(1.0f - law.bits) : 1.0f;
    const float holdHz = law.crush ? logTaper(t.holdMaxHz, t.holdMinHz, c) : t.holdMaxHz;
    law.holdRatio = clampf(holdHz / kSampleRate, kMinHoldRatio, 1.0f);
    return law;
  }

  // Index and fraction of `pos` on a grid of `points` points, 1 apart. A position past the last cell
  // stays on the last cell with a fraction over 1.
  static void gridPos(float pos, size_t points, size_t* i, float* frac) {
    size_t k = static_cast<size_t>(pos);
    if (k > points - 2) k = points - 2;
    *i = k;
    *frac = pos - static_cast<float>(k);
  }

  // The table in dB at TONE and the law's AMOUNT, linear between its points along both axes.
  static float makeupDbAt(float tone, float amount, const LofiTuning& t) {
    size_t ti, ai;
    float tf, af;
    gridPos(tone * static_cast<float>(kLofiMakeupTones - 1), kLofiMakeupTones, &ti, &tf);
    gridPos(amount * kLofiColumnsPerAmount, kLofiMakeupAmounts, &ai, &af);
    const auto row = [&](size_t r) { return t.makeupDb[r][ai] + af * (t.makeupDb[r][ai + 1] - t.makeupDb[r][ai]); };
    const float lo = row(ti);
    return lo + tf * (row(ti + 1) - lo);
  }

  // Values the loops ramp between. post is the inverse of the drive times the makeup; ratio is the held
  // rate over the sample rate.
  struct Gains {
    float drive, post, ratio;
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
    const float tone = smooth::clamp01(p.tone);
    const float amount = smooth::clamp01(p.amount) * kLofiAmountTop;
    if (onMix_ == 0.0f) {
      if (target == 0.0f) {
        for (int i = 0; i < n; ++i) out[i] = in[i];
        return;
      }
      clearVoice();
      tone_ = tone;
      amount_ = amount;
      lawValid_ = false;
    }

    const float coef = n == kBlock ? kBlockSmoothCoef : smooth::blockCoef(smooth::kSmoothSec, n);
    tone_ = smooth::step(tone_, tone, coef);
    amount_ = smooth::step(amount_, amount, coef);
    const bool tuningMoved = !sameWords(p.tuning, applied_);
    if (!lawValid_ || tone_ != lawTone_ || amount_ != lawAmount_ || tuningMoved) applyLaw(p.tuning, tuningMoved);
    play(in, out, n, target);
  }

  // The corner of the honk from sinCosFast. The two pass pairs keep dsp.h's setters and libm: the
  // high-pass corner sits near DC, where one ulp of cos(w0) moves the response under the corner by
  // 0.3 %, so its coefficients must round as libm rounds them.
  static RbjCorner cornerAt(float hz) {
    const float w0 = kTwoPi * clampf(hz, kFilterMinHz, kFilterMaxHz) / kSampleRate;
    RbjCorner k;
    sinCosFast(w0, k.sw, k.cw);
    return k;
  }

  // Sets the five filters' end-of-block coefficients and the block's end gains from the smoothed
  // knobs. The first law after a reset takes them at once; a later one is reached by the block's ramp.
  void applyLaw(const LofiTuning& t, bool tuningMoved) {
    if (tuningMoved || !lawValid_) logs_ = logsOf(t);
    law_ = lawOf(tone_, amount_, t, logs_);
    goal_[0].setHighPass(law_.hpHz, kQ1);
    goal_[1].setHighPass(law_.hpHz, kQ2);
    goal_[2].setLowPass(law_.lpHz, kQ1);
    goal_[3].setLowPass(law_.lpHz, kQ2);
    goal_[4].setPeakAmp(cornerAt(law_.fcHz), dbToGainFast(0.5f * law_.honkDb), t.honkQ);
    next_ = {law_.drive, law_.makeup / law_.drive, law_.holdRatio};
    if (lawValid_) {
      rampPending_ = true;
    } else {
      cur_ = next_;
      hp1_.setCoefsFrom(goal_[0]);
      hp2_.setCoefsFrom(goal_[1]);
      lp1_.setCoefsFrom(goal_[2]);
      lp2_.setCoefsFrom(goal_[3]);
      honk_.setCoefsFrom(goal_[4]);
      rampPending_ = false;
    }
    invStep_ = 1.0f / law_.step;
    lawTone_ = tone_;
    lawAmount_ = amount_;
    if (tuningMoved) applied_ = t;
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

  // The coefficients k samples into a block that began at `from`. Each is the block's start plus k
  // steps. Adding a step to the previous value would drop part of every step smaller than the
  // coefficient's last bit, so the ramp would end short of its goal and the snap onto the goal would
  // hand the section a step. With a low tone in the high-pass states and a quiet output, that step
  // reads as a click.
  static void rampTo(Biquad& b, const Biquad& from, const CoefStep& s, float k) {
    b.b0 = from.b0 + s.b0 * k;
    b.b1 = from.b1 + s.b1 * k;
    b.b2 = from.b2 + s.b2 * k;
    b.a1 = from.a1 + s.a1 * k;
    b.a2 = from.a2 + s.a2 * k;
  }

  // One section over the block, its coefficients ramping to the goal. x and y may be the same buffer.
  static void rampSection(Biquad& b, const Biquad& goal, const float* x, float* y, int n, float inv) {
    Biquad s = b;
    const Biquad from = b;
    const CoefStep step = stepsTo(from, goal, inv);
    float k = 1.0f;
    for (int i = 0; i < n; ++i, k += 1.0f) {
      rampTo(s, from, step, k);
      y[i] = s.run(x[i]);
    }
    s.setCoefsFrom(goal);  // land on the goal exactly, whatever the steps added up to
    b = s;
  }

  // The five sections in one loop, coefficients still.
  void bandSettled(const float* x, float* y, int n) {
    Biquad hp1 = hp1_, hp2 = hp2_, lp1 = lp1_, lp2 = lp2_, honk = honk_;
    for (int i = 0; i < n; ++i) y[i] = honk.run(lp2.run(lp1.run(hp2.run(hp1.run(x[i])))));
    hp1_ = hp1;
    hp2_ = hp2;
    lp1_ = lp1;
    lp2_ = lp2;
    honk_ = honk;
  }

  // A ramped section keeps its start, its step and its coefficients in registers, so each runs alone.
  void bandRamped(const float* x, float* y, int n, float inv) {
    rampSection(hp1_, goal_[0], x, y, n, inv);
    rampSection(hp2_, goal_[1], y, y, n, inv);
    rampSection(lp1_, goal_[2], y, y, n, inv);
    rampSection(lp2_, goal_[3], y, y, n, inv);
    rampSection(honk_, goal_[4], y, y, n, inv);
  }

  // The drive and tanh in place, two samples a pass so the two tanh chains overlap. The gain steps one
  // add a sample.
  static void driveBlock(float* v, int n, float g, float dg) {
    int i = 0;
    for (; i + 1 < n; i += 2) {
      const float g1 = g + dg;
      const float a = tanhCubic(v[i] * g);
      const float b = tanhCubic(v[i + 1] * g1);
      v[i] = a;
      v[i + 1] = b;
      g = g1 + dg;
    }
    if (i < n) v[i] = tanhCubic(v[i] * g);
  }

  // The sample-and-hold and the bit crush. A sample is taken whenever the phase wraps, and the value is
  // rounded to a multiple of the step. The + 0 turns the -0 of a negative value that rounds to zero into
  // +0. A NaN or an infinity in reaches the output, so the chain's guard sees it.
  struct Crusher {
    float step, invStep, phase, held;

    CV_INLINE float next(float v, float ratio) {
      phase += ratio;
      if (phase >= 1.0f) {
        phase -= 1.0f;
        held = step * std::round(v * invStep) + 0.0f;
      }
      return held;
    }
  };

  // True when nothing in the block moves: the gains sit on their goals and the stage is fully on.
  bool steady(float target) const {
    return cur_.drive == next_.drive && cur_.post == next_.post && cur_.ratio == next_.ratio && onMix_ == 1.0f &&
           target == 1.0f;
  }

  // The crush, the makeup and the dry/wet mix over the block's tanh output `w`.
  template <bool kSteady>
  void finish(const float* in, const float* w, float* out, int n, float target, float inv) {
    const bool crush = law_.crush;
    Crusher cr{law_.step, invStep_, phase_, held_};
    float post = cur_.post, ratio = cur_.ratio, mix = onMix_;
    const float dpost = (next_.post - post) * inv;
    const float dratio = (next_.ratio - ratio) * inv;
    if (kSteady && !crush) {
      for (int i = 0; i < n; ++i) out[i] = w[i] * post;
    } else {
      for (int i = 0; i < n; ++i) {
        const float v = (crush ? cr.next(w[i], ratio) : w[i]) * post;
        if constexpr (kSteady) {
          out[i] = v;
        } else {
          mix = smooth::step(mix, target, smooth::kSmoothCoef);
          out[i] = mix >= 1.0f ? v : in[i] + mix * (v - in[i]);
          post += dpost;
          ratio += dratio;
        }
      }
    }
    cur_ = next_;
    onMix_ = mix;

    // Without the crush the next block that needs it takes a fresh sample on its first sample.
    phase_ = crush ? cr.phase : 1.0f;
    held_ = crush ? cr.held : 0.0f;
  }

  // One block: the filters, the drive, then the crush and the mix.
  void play(const float* in, float* out, int n, float target) {
    std::array<float, kBlock> scratch;
    float* w = scratch.data();
    const float inv = 1.0f / static_cast<float>(n);
    if (rampPending_) {
      bandRamped(in, w, n, inv);
      rampPending_ = false;
    } else {
      bandSettled(in, w, n);
    }
    const float g = cur_.drive;
    driveBlock(w, n, g, (next_.drive - g) * inv);
    if (steady(target)) {
      finish<true>(in, w, out, n, target, inv);
    } else {
      finish<false>(in, w, out, n, target, inv);
    }
  }

  Biquad hp1_, hp2_, lp1_, lp2_, honk_;
  std::array<Biquad, 5> goal_;  // the coefficients the block ends on; the states are unused
  Law law_{};
  Logs logs_{};
  Gains cur_{1.0f, 1.0f, 1.0f};
  Gains next_{1.0f, 1.0f, 1.0f};
  LofiTuning applied_;
  float invStep_ = 1.0f;
  float tone_ = 0.5f;
  float amount_ = 0.0f;
  float lawTone_ = 0.0f;
  float lawAmount_ = 0.0f;
  float onMix_ = 0.0f;
  float phase_ = 1.0f;
  float held_ = 0.0f;
  bool lawValid_ = false;
  bool rampPending_ = false;  // a goal moved, so a section may sit off it
};

}  // namespace cv
