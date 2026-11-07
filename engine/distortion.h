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
// topBoostDb more over the top of the knob, from topBoostFrom to DRIVE 1. The two rail
// saturators run 2x oversampled. A rail is linear up to its soft edge, then a quadratic knee that
// meets the limit flat.
//
// A call runs in kBlock chunks, and each chunk runs stage by stage: one loop per run of filters, so
// each recursion keeps its state in registers, and the rails and half-bands between them work on
// linear histories in the engine's own scratch.

// The M7 has no SIMD. On the host, clang's vector body and its scalar tail round differently, which
// would make the output depend on the call size, so the chunk loops stay scalar.
#if defined(__clang__)
#define CV_NOVECTOR _Pragma("clang loop vectorize(disable) interleave(disable)")
#else
#define CV_NOVECTOR
#endif

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
  float trebleCutDb = -2.7f;      // -5.4 dB left the voice dark (owner 2026-11-01)
  float toneMinDb = -12.0f;       // TONE fully down: extra shelf gain above trebleCutHz
  float toneMaxDb = 6.0f;         // TONE fully up
  float bassPeakHz = 120.0f;      // gyrator bump
  float bassPeakDb = 2.0f;
  float bassPeakQ = 1.0f;
  float trimDb = 2.87f;           // on top of the makeup law; level rule
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
  // Tuning limits. applyTuning clamps to them and cubevox-render refuses a value outside them.
  static constexpr float kGainMin = 0.1f;
  static constexpr float kGainMax = 1000.0f;
  static constexpr float kDbLimit = 24.0f;
  static constexpr float kFadeDriveMin = 1e-4f;
  static constexpr float kRailAsymMax = 0.5f;
  static constexpr float kRailSoftMin = 0.01f;

  // State clears; the coefficients stay, because they depend on the tuning alone and process()
  // compares the tuning on every block.
  void reset() {
    clearChain();
    drive_ = 0.0f;
    fade_ = 0.0f;
    engaged_ = false;
  }

  void process(const float* CV_RESTRICT in, float* CV_RESTRICT out, int n,
               const DistortionParams& p) {
    const DistortionTuning& t = p.tuning;
    const float toneTarget = smooth::clamp01(p.tone);

    // The stage turns on when DRIVE reaches fadeDrive and off when it falls under kOffBelow of that,
    // and keeps its last state between the two. On, the smoother aims at the knob or at fadeDrive,
    // whichever is higher, and off it aims at 0, so the knob never parks the stage inside the fade.
    const float fade = clampf(t.fadeDrive, kFadeDriveMin, 1.0f);
    const float knobDrive = p.on ? smooth::clamp01(p.drive) : 0.0f;
    if (knobDrive >= fade) engaged_ = true;
    else if (knobDrive < kOffBelow * fade) engaged_ = false;
    const float target = engaged_ ? fmaxf(knobDrive, fade) : 0.0f;

    // With DRIVE at 0 and the fade out the wet path is silent and its filters are clear, so TONE follows
    // the knob and the stage starts from where the knob sits.
    const bool dry = drive_ == 0.0f && fade_ == 0.0f;
    if (dry) tone_ = toneTarget;

    // Settled drive 0 skips the chain. State clears once so re-entry starts clean.
    if (dry && target == 0.0f) {
      if (dirty_) clearChain();
      for (int i = 0; i < n; ++i) out[i] = in[i];
      return;
    }
    dirty_ = true;

    if (!tuningValid_ || !sameWords(t, applied_)) applyTuning(t);
    for (int off = 0; off < n; off += kBlock) {
      const int m = n - off < kBlock ? n - off : kBlock;
      runChunk(in + off, out + off, m, target, toneTarget);
    }

    // A NaN or infinity in the input lands in the stage 1 filters first, and the rails turn it into 0
    // before it reaches the later ones. It would stay in the filters for good, so the chain starts over.
    if (!chain_.finite()) clearChain();
  }

  // Stage 1 gain at DRIVE d for the tuning the last process() call applied.
  float stage1GainAt(float d) const { return gainsAt(d).g1; }

 private:
  // OnePole::set takes no corner under kFilterMinHz, so 20 Hz is the lowest the blocker can have.
  static constexpr float kDcBlockHz = 20.0f;

  // The stage turns off once DRIVE falls under this share of fadeDrive (4 % at the default 5 %).
  static constexpr float kOffBelow = 0.8f;

  // The mix with the dry input moves toward its goal, smoothstep(clamp(DRIVE / fadeDrive, 0, 1)), by at
  // most one step a sample, so a fade takes at least kFadeMs whichever way it goes and the wet path's
  // filters settle before it is heard in full. The goal's zero slope at the end of the fade keeps the
  // mix from kinking where DRIVE leaves it.
  static constexpr float kFadeMs = 16.0f;
  static constexpr float kFadeStep = 1.0f / (kFadeMs * kSmpPerMs);

  // Smallest DRIVE span the top boost ramp spreads over, so topBoostFrom at 1 never divides by 0.
  static constexpr float kBoostSpanMin = 1e-4f;

  static float boundDb(float db) { return clampf(db, -kDbLimit, kDbLimit); }

  // Stage gains and the output scale at one DRIVE value.
  struct Gains {
    float g1, g2, k;
  };

  // The top boost ramps in dB, so it adds to the stage 1 exponent. Below topBoostFrom the
  // added term is exactly 0.0f and the gain equals the plain exponential sweep bit for bit.
  Gains gainsAt(float drive) const {
    const float over = drive > boostFrom_ ? drive - boostFrom_ : 0.0f;
    const float g1 = g1Min_ * exp2Fast(log2R1_ * drive + boostK_ * over);
    const float g2 = g2Min_ * exp2Fast(log2R2_ * drive);
    return {g1, g2, trim_ / makeup(g1 * g2)};
  }

  // The gains at the chunk-start drive, kept for the drive they were computed at.
  Gains startGains() {
    if (gainDrive_ != drive_) {
      gainCache_ = gainsAt(drive_);
      gainDrive_ = drive_;
    }
    return gainCache_;
  }

  // makeup(g) = a*g / sqrt(1 + (a*g / s)^2): follows the linear stage gain (a = 0.06) and flattens at
  // the saturated level (s = 12.53). With trimDb it is fitted to Adam's three clips, so their mean level
  // holds within 0.35 dB of 0.25 dB over the input from DRIVE 10 % to 100 % at TONE 50 %.
  static float makeup(float gain) {
    const float lin = kMakeupLinear * gain;
    const float r = lin / kMakeupSat;
    return lin / sqrtf(1.0f + r * r);
  }
  static constexpr float kMakeupLinear = 0.06f;
  static constexpr float kMakeupSat = 12.53f;

  // Half-band FIR, 15 taps, Hamming window. Taps at even offsets are zero and the centre is 0.5, so
  // each phase needs only the few taps below. They are scaled so the odd taps (each of these twice)
  // sum to 0.5 and the filter has unity gain at DC.
  static constexpr float kH1 = 0.30608729f;
  static constexpr float kH3 = -0.06866084f;
  static constexpr float kH5 = 0.01623882f;
  static constexpr float kH7 = -0.00366527f;

  // TONE maps min..0 dB over the lower half and 0..max dB over the upper half, so
  // noon adds nothing.
  static float toneDb(const DistortionTuning& t, float tone) {
    return tone < 0.5f ? boundDb(t.toneMinDb) * (1.0f - 2.0f * tone)
                       : boundDb(t.toneMaxDb) * (2.0f * tone - 1.0f);
  }

  // Recomputes everything that depends on tuning alone.
  void applyTuning(const DistortionTuning& t) {
    chain_.inHp.set(t.inputHpHz);
    chain_.s1Lp.set(t.s1LpHz);
    chain_.s2Hp.set(t.s2HpHz);
    chain_.s2Lp.set(t.s2LpHz);
    chain_.dc.set(kDcBlockHz);
    chain_.s1Bass.setLowShelf(t.s1BassHz, boundDb(t.s1BassDb));
    chain_.stackBass.setLowShelf(t.stackBassHz, boundDb(t.stackBassDb));
    chain_.stackTreble.setHighShelf(t.stackTrebleHz, boundDb(t.stackTrebleDb));
    chain_.bassPeak.setPeak(t.bassPeakHz, boundDb(t.bassPeakDb), t.bassPeakQ);
    rail_ = makeRail(1.0f - clampf(t.railAsym, 0.0f, kRailAsymMax), clampf(t.railSoft, kRailSoftMin, 1.0f));
    stackLoss_ = dbToGain(boundDb(t.stackLossDb));
    trim_ = dbToGain(boundDb(t.trimDb));
    g1Min_ = clampf(t.gain1Min, kGainMin, kGainMax);
    g2Min_ = clampf(t.gain2Min, kGainMin, kGainMax);
    log2R1_ = log2f(clampf(t.gain1Max, g1Min_, kGainMax) / g1Min_);
    log2R2_ = log2f(clampf(t.gain2Max, g2Min_, kGainMax) / g2Min_);
    boostFrom_ = smooth::clamp01(t.topBoostFrom);
    const float boostSpan = fmaxf(1.0f - boostFrom_, kBoostSpanMin);
    boostK_ = clampf(t.topBoostDb, 0.0f, kDbLimit) * kLog2PerDb / boostSpan;
    fadeSpan_ = clampf(t.fadeDrive, kFadeDriveMin, 1.0f);
    invFadeSpan_ = 1.0f / fadeSpan_;
    shelfCorner_ = rbjCorner(t.trebleCutHz);
    applied_ = t;
    tuningValid_ = true;
    shelfTone_ = -1.0f;
    gainDrive_ = -1.0f;
  }

  // The treble cut's high shelf at TONE `tone`: Biquad::setHighShelfAmp's RBJ design at slope 1 with
  // one reciprocal in place of its five divides. The corner comes cached from applyTuning and the gain
  // through exp2Fast, so a glide costs no libm.
  Biquad shelfAt(float tone) const {
    const float db = boundDb(applied_.trebleCutDb) + toneDb(applied_, tone);
    const float amp = dbToGainFast(0.5f * db);  // 10^(db / 40)
    const float ap = amp + 1.0f, am = amp - 1.0f;
    const float beta = 2.0f * sqrtf(amp) * (shelfCorner_.sw * kButterworthQ);
    const float apc = ap * shelfCorner_.cw, amc = am * shelfCorner_.cw;
    const float r = 1.0f / (ap - amc + beta);
    Biquad s;
    s.b0 = amp * (ap + amc + beta) * r;
    s.b1 = -2.0f * amp * (am + apc) * r;
    s.b2 = amp * (ap + amc - beta) * r;
    s.a1 = 2.0f * (am - apc) * r;
    s.a2 = (ap - amc - beta) * r;
    return s;
  }

 public:
  // The rail and its parts are public because the Growl card clips with the same knee.
  //
  // One side of the rail. The soft edge is `edge` wide and ends at the limit; the knee starts at
  // `start` = limit - edge, runs for span = 2 edge and ends at `end` = limit + edge, and
  // curve = 1 / (4 edge) is its curvature.
  struct Side {
    float start, span, curve, end;
  };

  // Positive rail 1, negative rail -(1 - asym).
  struct Rail {
    Side pos, neg;
  };

  static Side makeSide(float limit, float soft) {
    const float edge = soft * limit;
    return Side{limit - edge, 2.0f * edge, 0.25f / edge, limit + edge};
  }

  static Rail makeRail(float neg, float soft) { return Rail{makeSide(1.0f, soft), makeSide(neg, soft)}; }

  // One side's curve for v >= 0: linear up to the start, then the quadratic knee v - t^2 / (4 edge)
  // over two edge widths, which meets the limit with zero slope and stays there. With w the part of the
  // distance past the start that lies inside the knee, the curve is min(v, end) - curve * w^2, and min
  // and max pick the segment, so it has no branch. Past the knee it is the constant end - curve *
  // span^2 for any v, so a huge input cannot cancel against itself.
  static CV_INLINE float knee(const Side& s, float v) {
    const float w = fminf(fmaxf(v - s.start, 0.0f), s.span);
    return fminf(v, s.end) - s.curve * (w * w);
  }

  // The rail. The positive part of x runs through the positive side and the negative part through the
  // negative side, and one of them is always 0, so the curve needs no branch, no compare and no select.
  // A NaN input reads as 0.
  static CV_INLINE float rail(const Rail& r, float x) {
    return knee(r.pos, fmaxf(x, 0.0f)) - knee(r.neg, fmaxf(-x, 0.0f));
  }

 private:
  // The half-bands read 8 samples, so the rail's input and even output keep 7 samples of history ahead
  // of the chunk, and the odd output 4.
  static constexpr int kLead = 7;
  static constexpr int kOddLead = 4;

  // A half-band output over h[0..7], h[7] the newest.
  static CV_INLINE float halfBand(const float* h) {
    return kH7 * (h[7] + h[0]) + kH5 * (h[6] + h[1]) + kH3 * (h[5] + h[2]) + kH1 * (h[4] + h[3]);
  }

  // The scratch of one 2x oversampled rail over a chunk. Sample i of the chunk sits at u[kLead + i],
  // e[kLead + i] and o[kOddLead + i]. u is the rail's input, e and o its outputs at the even and
  // odd phases, and y the half-band's downsampled result.
  struct RailLines {
    std::array<float, kLead + kBlock> u{};
    std::array<float, kLead + kBlock> e{};
    std::array<float, kOddLead + kBlock> o{};
    std::array<float, kBlock> y{};

    void clear() {
      for (int k = 0; k < kLead; ++k) u[static_cast<size_t>(k)] = e[static_cast<size_t>(k)] = 0.0f;
      for (int k = 0; k < kOddLead; ++k) o[static_cast<size_t>(k)] = 0.0f;
    }

    // Moves the last kLead (kOddLead) samples of an n-sample chunk to the front.
    void carry(int n) {
      for (int k = 0; k < kLead; ++k) {
        u[static_cast<size_t>(k)] = u[static_cast<size_t>(n + k)];
        e[static_cast<size_t>(k)] = e[static_cast<size_t>(n + k)];
      }
      for (int k = 0; k < kOddLead; ++k) o[static_cast<size_t>(k)] = o[static_cast<size_t>(n + k)];
    }
  };

  // The rail over a chunk at 2x: the even phase is the half-band's interpolated sample, the odd phase
  // the centre tap, and the down pair brings them back to the base rate.
  static void railPass(RailLines& L, const Rail& railIn, int n) {
    const Rail r = railIn;  // a local copy, so the stores below cannot alias its edges
    float* CV_RESTRICT e = L.e.data();
    float* CV_RESTRICT o = L.o.data();
    const float* CV_RESTRICT u = L.u.data();
    CV_NOVECTOR
    for (int i = 0; i < n; ++i) {
      e[kLead + i] = rail(r, 2.0f * halfBand(u + i));
      o[kOddLead + i] = rail(r, u[4 + i]);
    }
    float* CV_RESTRICT y = L.y.data();
    CV_NOVECTOR
    for (int i = 0; i < n; ++i) y[i] = halfBand(e + i) + 0.5f * o[i];
  }

  // The signal path's filters.
  struct Chain {
    OnePole inHp, s1Lp, s2Hp, s2Lp, dc;
    Biquad s1Bass, stackBass, stackTreble, trebleCut, bassPeak;

    void clear() {
      for (OnePole* p : {&inHp, &s1Lp, &s2Hp, &s2Lp, &dc}) p->clear();
      for (Biquad* b : {&s1Bass, &stackBass, &stackTreble, &trebleCut, &bassPeak}) b->clear();
    }

    // Every state is finite. A NaN or infinity anywhere makes the sum non-finite, because the states
    // of a healthy chain stay far under the float range.
    bool finite() const {
      float sum = 0.0f;
      for (const OnePole* p : {&inHp, &s1Lp, &s2Hp, &s2Lp, &dc}) sum += p->y;
      for (const Biquad* b : {&s1Bass, &stackBass, &stackTreble, &trebleCut, &bassPeak}) sum += b->z1 + b->z2;
      return std::isfinite(sum);
    }
  };

  // Input shaping and stage 1 gain, into the first rail's input. The gain is evaluated at the chunk
  // edges and interpolated across it.
  void stage1(const float* CV_RESTRICT in, int n, const Gains& a, const Gains& b, float inv, bool settled) {
    OnePole inHp = chain_.inHp, s1Lp = chain_.s1Lp;
    Biquad s1Bass = chain_.s1Bass;
    float* CV_RESTRICT u = r1_.u.data() + kLead;
    if (settled) {
      for (int i = 0; i < n; ++i) u[i] = s1Lp.lp(s1Bass.run(inHp.hp(in[i]))) * a.g1;
    } else {
      for (int i = 0; i < n; ++i) {
        const float fi = static_cast<float>(i + 1) * inv;
        u[i] = s1Lp.lp(s1Bass.run(inHp.hp(in[i]))) * (a.g1 + (b.g1 - a.g1) * fi);
      }
    }
    chain_.inHp = inHp;
    chain_.s1Lp = s1Lp;
    chain_.s1Bass = s1Bass;
  }

  // The first rail's output through the tone stack and the stage 2 shaping and gain, into the second
  // rail's input.
  void stage2(int n, const Gains& a, const Gains& b, float inv, bool settled) {
    Biquad stackBass = chain_.stackBass, stackTreble = chain_.stackTreble;
    OnePole s2Hp = chain_.s2Hp, s2Lp = chain_.s2Lp;
    const float stackLoss = stackLoss_;
    const float* CV_RESTRICT y = r1_.y.data();
    float* CV_RESTRICT u = r2_.u.data() + kLead;
    if (settled) {
      for (int i = 0; i < n; ++i) {
        const float x = stackTreble.run(stackBass.run(y[i])) * stackLoss;
        u[i] = s2Lp.lp(s2Hp.hp(x)) * a.g2;
      }
    } else {
      for (int i = 0; i < n; ++i) {
        const float x = stackTreble.run(stackBass.run(y[i])) * stackLoss;
        const float fi = static_cast<float>(i + 1) * inv;
        u[i] = s2Lp.lp(s2Hp.hp(x)) * (a.g2 + (b.g2 - a.g2) * fi);
      }
    }
    chain_.stackBass = stackBass;
    chain_.stackTreble = stackTreble;
    chain_.s2Hp = s2Hp;
    chain_.s2Lp = s2Lp;
  }

  // The second rail's output through the post shaping and makeup, with everything settled and DRIVE
  // at or above fadeDrive: the shelf and the makeup are constants.
  void stage3Settled(float* CV_RESTRICT out, int n, float k, const Biquad& s) {
    Biquad trebleCut = chain_.trebleCut, bassPeak = chain_.bassPeak;
    OnePole dc = chain_.dc;
    trebleCut.setCoefsFrom(s);
    const float* CV_RESTRICT y = r2_.y.data();
    for (int i = 0; i < n; ++i) out[i] = dc.hp(bassPeak.run(trebleCut.run(y[i]))) * k;
    chain_.trebleCut = trebleCut;
    chain_.bassPeak = bassPeak;
    chain_.dc = dc;
  }

  // How a chunk mixes the wet path with the dry input: not at all, a fade still rising to the full chain
  // with DRIVE past fadeDrive for the whole chunk, or a fade that follows DRIVE through its first
  // fadeDrive a sample at a time.
  enum Mix { kWetOnly, kRiseToWet, kFollowDrive };

  // What one chunk takes from the knobs: the stage gains and the treble cut at its start (a, sa) and
  // end (b, sb), 1 / n, the DRIVE and TONE it ends on, and how they move.
  struct Chunk {
    int n;
    float inv;
    Gains a, b;
    Biquad sa, sb;
    float driveEnd, toneEnd;
    bool driveMoves, toneMoves;
    Mix mix;
  };

  // The second rail's output through the post shaping and makeup, with the makeup interpolated across
  // the chunk, the treble shelf interpolated from sa to sb while TONE moves (kShelfMoves), and the mix
  // with the dry input (kMix).
  template <bool kShelfMoves, Mix kMix>
  void stage3Moving(const float* CV_RESTRICT in, float* CV_RESTRICT out, const Chunk& c) {
    Biquad trebleCut = chain_.trebleCut, bassPeak = chain_.bassPeak;
    OnePole dc = chain_.dc;
    const Gains& a = c.a;
    const Gains& b = c.b;
    const Biquad& sa = c.sa;
    const Biquad& sb = c.sb;
    trebleCut.setCoefsFrom(sa);
    const float* CV_RESTRICT y = r2_.y.data();
    const float* CV_RESTRICT driveLine = driveLine_.data();
    float fade = fade_;
    for (int i = 0; i < c.n; ++i) {
      const float fi = static_cast<float>(i + 1) * c.inv;
      if constexpr (kShelfMoves) {
        trebleCut.b0 = sa.b0 + (sb.b0 - sa.b0) * fi;
        trebleCut.b1 = sa.b1 + (sb.b1 - sa.b1) * fi;
        trebleCut.b2 = sa.b2 + (sb.b2 - sa.b2) * fi;
        trebleCut.a1 = sa.a1 + (sb.a1 - sa.a1) * fi;
        trebleCut.a2 = sa.a2 + (sb.a2 - sa.a2) * fi;
      }
      const float wet = dc.hp(bassPeak.run(trebleCut.run(y[i]))) * (a.k + (b.k - a.k) * fi);
      if constexpr (kMix == kFollowDrive) {
        const float x = fminf(driveLine[i] * invFadeSpan_, 1.0f);
        const float goal = x * x * (3.0f - 2.0f * x);
        fade += clampf(goal - fade, -kFadeStep, kFadeStep);
        out[i] = fade >= 1.0f ? wet : in[i] + fade * (wet - in[i]);
      } else if constexpr (kMix == kRiseToWet) {
        fade += fminf(1.0f - fade, kFadeStep);
        out[i] = fade >= 1.0f ? wet : in[i] + fade * (wet - in[i]);
      } else {
        out[i] = wet;
      }
    }
    if constexpr (kMix != kWetOnly) fade_ = fade;
    chain_.trebleCut = trebleCut;
    chain_.bassPeak = bassPeak;
    chain_.dc = dc;
  }

  template <bool kShelfMoves>
  void stage3Mixed(const float* CV_RESTRICT in, float* CV_RESTRICT out, const Chunk& c) {
    if (c.mix == kWetOnly) {
      stage3Moving<kShelfMoves, kWetOnly>(in, out, c);
    } else if (c.mix == kRiseToWet) {
      stage3Moving<kShelfMoves, kRiseToWet>(in, out, c);
    } else {
      stage3Moving<kShelfMoves, kFollowDrive>(in, out, c);
    }
  }

  // Everything settled takes the constant path; any other chunk takes the variant its TONE and mix call for.
  void stage3(const float* CV_RESTRICT in, float* CV_RESTRICT out, const Chunk& c) {
    if (!c.driveMoves && !c.toneMoves && c.mix == kWetOnly) {
      stage3Settled(out, c.n, c.a.k, c.sa);
    } else if (c.toneMoves) {
      stage3Mixed<true>(in, out, c);
    } else {
      stage3Mixed<false>(in, out, c);
    }
  }

  // Steps DRIVE toward target a sample at a time, keeps each sample's value for stage 3, and returns
  // the DRIVE it ends on.
  float rampDrive(float target, int n) {
    float drive = drive_;
    for (int i = 0; i < n; ++i) {
      drive = smooth::step(drive, target, smooth::kSmoothCoef);
      driveLine_[static_cast<size_t>(i)] = drive;
    }
    return drive;
  }

  // The values one chunk of n samples runs on.
  Chunk planChunk(int n, float target, float toneTarget) {
    Chunk c{};
    c.n = n;
    c.inv = 1.0f / static_cast<float>(n);
    c.driveMoves = drive_ != target;
    c.toneMoves = tone_ != toneTarget;
    c.driveEnd = drive_;
    c.toneEnd = tone_;
    if (c.driveMoves || c.toneMoves) {
      if (n == kBlock) {
        c.driveEnd = smooth::advanceBlock(drive_, target);
        c.toneEnd = smooth::advanceBlock(tone_, toneTarget);
      } else {
        smooth::advance(c.driveEnd, target, c.toneEnd, toneTarget, smooth::kSmoothCoef, n);
      }
    }
    // DRIVE moves one way, so a chunk with both ends past fadeDrive never has a sample inside it. A chunk
    // that does steps DRIVE a sample at a time, and its end value is that loop's.
    const bool pastFade = drive_ >= fadeSpan_ && c.driveEnd >= fadeSpan_;
    c.mix = pastFade ? (fade_ < 1.0f ? kRiseToWet : kWetOnly) : kFollowDrive;
    if (!pastFade) c.driveEnd = rampDrive(target, n);
    c.a = startGains();
    c.b = c.driveMoves ? gainsAt(c.driveEnd) : c.a;
    c.sa = shelf_;
    c.sb = c.toneMoves ? shelfAt(c.toneEnd) : c.sa;
    return c;
  }

  // One chunk of at most kBlock samples.
  void runChunk(const float* CV_RESTRICT in, float* CV_RESTRICT out, int n, float target, float toneTarget) {
    if (tone_ != shelfTone_) {
      shelf_ = shelfAt(tone_);
      shelfTone_ = tone_;
    }
    const Chunk c = planChunk(n, target, toneTarget);

    stage1(in, n, c.a, c.b, c.inv, !c.driveMoves);
    railPass(r1_, rail_, n);
    stage2(n, c.a, c.b, c.inv, !c.driveMoves);
    railPass(r2_, rail_, n);
    stage3(in, out, c);
    r1_.carry(n);
    r2_.carry(n);

    drive_ = c.driveEnd;
    tone_ = c.toneEnd;
    shelf_ = c.sb;
    shelfTone_ = c.toneEnd;
    gainCache_ = c.b;
    gainDrive_ = c.driveEnd;
  }

  void clearChain() {
    chain_.clear();
    r1_.clear();
    r2_.clear();
    dirty_ = false;
  }

  Chain chain_;
  RailLines r1_, r2_;
  float drive_ = 0.0f;
  float tone_ = 0.5f;
  float fade_ = 0.0f;  // the wet share of the output: 0 is the dry input, 1 the chain alone
  bool dirty_ = false;
  bool engaged_ = false;  // the stage is on; DRIVE between the on and off thresholds leaves it as it was

  DistortionTuning applied_;
  bool tuningValid_ = false;
  Biquad shelf_;  // the treble cut at TONE shelfTone_
  float shelfTone_ = -1.0f;
  Rail rail_{{0.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 1.0f, 1.0f}};
  Gains gainCache_{1.0f, 1.0f, 1.0f};
  float gainDrive_ = -1.0f;  // the DRIVE gainCache_ was computed at
  float stackLoss_ = 1.0f, trim_ = 1.0f, g1Min_ = 1.0f, g2Min_ = 1.0f;
  float log2R1_ = 0.0f, log2R2_ = 0.0f;  // the octaves each stage gain spans over DRIVE 0 to 1
  float fadeSpan_ = 1.0f, invFadeSpan_ = 1.0f;  // fadeDrive and its reciprocal
  float boostFrom_ = 1.0f, boostK_ = 0.0f;  // top boost: start DRIVE, octaves per unit DRIVE past it
  RbjCorner shelfCorner_{1.0f, 0.0f};  // the treble cut's corner
  std::array<float, kBlock> driveLine_{};  // DRIVE at each sample of a chunk inside the fade
};

}  // namespace cv
