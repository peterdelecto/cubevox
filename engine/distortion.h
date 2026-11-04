#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

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
  float trebleCutDb = -2.7f;      // half the BD-2's -5.4 dB, which left the voice dark (owner 2026-11-01)
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

  // State clears; the coefficients stay, because they depend on the tuning alone and process()
  // compares the tuning on every block.
  void reset() {
    clearChain();
    drive_ = 0.0f;
    fade_ = 0.0f;
    tone_ = 0.5f;
    engaged_ = false;
    shelfTone_ = -1.0f;
  }

  void process(const float* CV_RESTRICT in, float* CV_RESTRICT out, int n,
               const DistortionParams& p) {
    const DistortionTuning& t = p.tuning;
    const float toneTarget = knob(p.tone);

    // The stage turns on when DRIVE reaches fadeDrive and off when it falls under kOffBelow of that,
    // and keeps its last state between the two. On, the smoother aims at the knob or at fadeDrive,
    // whichever is higher, and off it aims at 0, so the knob never parks the stage inside the fade.
    const float fade = bound(t.fadeDrive, kFadeDriveMin, 1.0f);
    const float knobDrive = p.on ? knob(p.drive) : 0.0f;
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

    if (!tuningValid_ || !same(t, applied_)) applyTuning(t);
    for (int off = 0; off < n; off += kBlock) {
      const int m = n - off < kBlock ? n - off : kBlock;
      runChunk(in + off, out + off, m, target, toneTarget);
    }

    // A non-finite input leaves NaN in the filters for good, so the chain starts over.
    if (!stateFinite()) clearChain();
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

  // x inside [lo, hi]. A NaN reads as lo, because fmaxf and fminf return the other argument.
  static float bound(float x, float lo, float hi) { return fminf(fmaxf(x, lo), hi); }
  static float knob(float x) { return bound(x, 0.0f, 1.0f); }
  static float boundDb(float db) { return bound(db, -kDbLimit, kDbLimit); }

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

  static float dbToLin(float db) { return powf(10.0f, db / 20.0f); }
  // TONE maps min..0 dB over the lower half and 0..max dB over the upper half, so
  // noon adds nothing.
  static float toneDb(const DistortionTuning& t, float tone) {
    return tone < 0.5f ? boundDb(t.toneMinDb) * (1.0f - 2.0f * tone)
                       : boundDb(t.toneMaxDb) * (2.0f * tone - 1.0f);
  }

  // The tuning is 28 floats with no padding, so equal words mean an equal tuning. A NaN field reads
  // as unchanged. The words are compared inline: the board's memcmp is a byte loop.
  static constexpr size_t kTuningWords = 28;
  static_assert(std::is_trivially_copyable<DistortionTuning>::value &&
                    sizeof(DistortionTuning) == kTuningWords * sizeof(uint32_t),
                "same() compares the tuning as 28 words");
  static bool same(const DistortionTuning& a, const DistortionTuning& b) {
    uint32_t diff = 0;
    for (size_t k = 0; k < kTuningWords; ++k) {
      uint32_t x, y;
      std::memcpy(&x, reinterpret_cast<const char*>(&a) + k * sizeof x, sizeof x);
      std::memcpy(&y, reinterpret_cast<const char*>(&b) + k * sizeof y, sizeof y);
      diff |= x ^ y;
    }
    return diff == 0;
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
    rail_ = makeRail(1.0f - bound(t.railAsym, 0.0f, 0.5f), bound(t.railSoft, 0.01f, 1.0f));
    stackLoss_ = dbToLin(boundDb(t.stackLossDb));
    trim_ = dbToLin(boundDb(t.trimDb));
    g1Min_ = bound(t.gain1Min, kGainMin, kGainMax);
    g2Min_ = bound(t.gain2Min, kGainMin, kGainMax);
    log2R1_ = log2f(bound(t.gain1Max, g1Min_, kGainMax) / g1Min_);
    log2R2_ = log2f(bound(t.gain2Max, g2Min_, kGainMax) / g2Min_);
    boostFrom_ = bound(t.topBoostFrom, 0.0f, 1.0f);
    const float boostRoom = 1.0f - boostFrom_;
    const float boostSpan = boostRoom < kFadeDriveMin ? kFadeDriveMin : boostRoom;
    boostK_ = bound(t.topBoostDb, 0.0f, kDbLimit) * kLog2PerDb / boostSpan;
    fadeSpan_ = bound(t.fadeDrive, kFadeDriveMin, 1.0f);
    const float w0 = kTwoPi * clampf(t.trebleCutHz, kFilterMinHz, kFilterMaxHz) / kSampleRate;
    shelfCos_ = cosf(w0);
    shelfAlpha_ = sinf(w0) * kButterworthQ;
    applied_ = t;
    tuningValid_ = true;
    shelfTone_ = -1.0f;
    gainDrive_ = -1.0f;
  }

  // The coefficients of a biquad section.
  struct Shelf {
    float b0, b1, b2, a1, a2;
  };

  // The treble cut's high shelf at TONE `tone`: Biquad::setHighShelf's RBJ design at slope 1, with the
  // corner's cosine and sine cached by applyTuning and the gain through exp2Fast, so a glide costs one
  // square root and one divide a chunk and no libm.
  Shelf shelfAt(float tone) const {
    const DistortionTuning& t = applied_;
    const float db = boundDb(t.trebleCutDb) + toneDb(t, tone);
    const float amp = dbToGainFast(0.5f * db);  // 10^(db / 40)
    const float ap = amp + 1.0f, am = amp - 1.0f;
    const float beta = 2.0f * sqrtf(amp) * shelfAlpha_;
    const float apc = ap * shelfCos_, amc = am * shelfCos_;
    const float r = 1.0f / (ap - amc + beta);
    return {amp * (ap + amc + beta) * r, -2.0f * amp * (am + apc) * r, amp * (ap + amc - beta) * r,
            2.0f * (am - apc) * r, (ap - amc - beta) * r};
  }

 public:
  // The rail and its parts are public because the Growl card clips with the same knee.
  //
  // One side of the rail. The soft edge is `edge` wide and ends at the limit; the knee starts at
  // `start` = limit - edge and runs for span = 2 edge, and curve = 1 / (4 edge) is its curvature.
  struct Side {
    float start, span, curve;
  };

  // Positive rail 1, negative rail -(1 - asym).
  struct Rail {
    Side pos, neg;
  };

  static Side makeSide(float limit, float soft) {
    const float edge = soft * limit;
    return Side{limit - edge, 2.0f * edge, 0.25f / edge};
  }

  static Rail makeRail(float neg, float soft) { return Rail{makeSide(1.0f, soft), makeSide(neg, soft)}; }

  // One side's curve for v >= 0: linear up to the start, then the quadratic knee v - t^2 / (4 edge)
  // over two edge widths, which meets the limit with zero slope and stays there. With t the distance
  // past the start and w the part of it inside the knee, the curve is v - (t - w) - curve * w^2, and
  // min and max pick the segment, so it has no branch.
  static CV_INLINE float knee(const Side& s, float v) {
    const float t = fmaxf(v - s.start, 0.0f);
    const float w = fminf(t, s.span);
    return v - (t - w) - s.curve * (w * w);
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

  // The signal path's filters. Stage gains g1 and g2 come from DRIVE.
  struct Chain {
    OnePole inHp, s1Lp, s2Hp, s2Lp, dc;
    Biquad s1Bass, stackBass, stackTreble, trebleCut, bassPeak;

    void clear() {
      for (OnePole* p : {&inHp, &s1Lp, &s2Hp, &s2Lp, &dc}) p->clear();
      for (Biquad* b : {&s1Bass, &stackBass, &stackTreble, &trebleCut, &bassPeak}) b->clear();
    }
  };

  // Input shaping and stage 1 gain, into the first rail's input. The stage gains and makeup are
  // evaluated at the chunk edges and interpolated across it.
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
  void stage3Settled(float* CV_RESTRICT out, int n, float k, const Shelf& s) {
    Biquad trebleCut = chain_.trebleCut, bassPeak = chain_.bassPeak;
    OnePole dc = chain_.dc;
    setCoefs(trebleCut, s);
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

  // The same with the makeup interpolated across the chunk, the treble shelf interpolated from `sa` to
  // `sb` while TONE moves (kShelfMoves), and the mix with the dry input (kMix).
  template <bool kShelfMoves, Mix kMix>
  void stage3Moving(const float* CV_RESTRICT in, float* CV_RESTRICT out, int n, const Gains& a, const Gains& b,
                    float inv, const Shelf& sa, const Shelf& sb, float target) {
    Biquad trebleCut = chain_.trebleCut, bassPeak = chain_.bassPeak;
    OnePole dc = chain_.dc;
    setCoefs(trebleCut, sa);
    const float* CV_RESTRICT y = r2_.y.data();
    const float fadeSpan = fadeSpan_;
    float drive = drive_, fade = fade_;
    for (int i = 0; i < n; ++i) {
      const float fi = static_cast<float>(i + 1) * inv;
      if constexpr (kShelfMoves) {
        trebleCut.b0 = sa.b0 + (sb.b0 - sa.b0) * fi;
        trebleCut.b1 = sa.b1 + (sb.b1 - sa.b1) * fi;
        trebleCut.b2 = sa.b2 + (sb.b2 - sa.b2) * fi;
        trebleCut.a1 = sa.a1 + (sb.a1 - sa.a1) * fi;
        trebleCut.a2 = sa.a2 + (sb.a2 - sa.a2) * fi;
      }
      const float wet = dc.hp(bassPeak.run(trebleCut.run(y[i]))) * (a.k + (b.k - a.k) * fi);
      if constexpr (kMix == kFollowDrive) {
        drive = smooth::step(drive, target, smooth::kSmoothCoef);
        const float x = fminf(drive / fadeSpan, 1.0f);
        const float goal = x * x * (3.0f - 2.0f * x);
        fade += fminf(fmaxf(goal - fade, -kFadeStep), kFadeStep);
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

  static void setCoefs(Biquad& bq, const Shelf& s) {
    bq.b0 = s.b0;
    bq.b1 = s.b1;
    bq.b2 = s.b2;
    bq.a1 = s.a1;
    bq.a2 = s.a2;
  }

  // One chunk of at most kBlock samples.
  void runChunk(const float* CV_RESTRICT in, float* CV_RESTRICT out, int n, float target, float toneTarget) {
    if (tone_ != shelfTone_) {
      shelf_ = shelfAt(tone_);
      shelfTone_ = tone_;
    }
    const bool driveMoves = drive_ != target;
    const bool toneMoves = tone_ != toneTarget;
    float driveEnd = drive_, toneEnd = tone_;
    if (driveMoves || toneMoves) {
      if (n == kBlock) {
        driveEnd = smooth::advanceBlock(drive_, target);
        toneEnd = smooth::advanceBlock(tone_, toneTarget);
      } else {
        smooth::advance(driveEnd, target, toneEnd, toneTarget, smooth::kSmoothCoef, n);
      }
    }
    // DRIVE moves one way, so a chunk with both ends past fadeDrive never has a sample inside it. A chunk
    // that does steps DRIVE a sample at a time, and its end value is that loop's.
    const bool pastFade = drive_ >= fadeSpan_ && driveEnd >= fadeSpan_;
    if (!pastFade && driveMoves) driveEnd = smooth::advance(drive_, target, smooth::kSmoothCoef, n);
    const Mix mix = !pastFade ? kFollowDrive : (fade_ < 1.0f ? kRiseToWet : kWetOnly);
    const Gains a = startGains();
    const Gains b = driveMoves ? gainsAt(driveEnd) : a;
    const float inv = 1.0f / static_cast<float>(n);
    const Shelf sa = shelf_;
    const Shelf sb = toneMoves ? shelfAt(toneEnd) : sa;

    stage1(in, n, a, b, inv, !driveMoves);
    railPass(r1_, rail_, n);
    stage2(n, a, b, inv, !driveMoves);
    railPass(r2_, rail_, n);
    if (!driveMoves && !toneMoves && mix == kWetOnly) {
      stage3Settled(out, n, a.k, sa);
    } else if (toneMoves) {
      if (mix == kWetOnly) stage3Moving<true, kWetOnly>(in, out, n, a, b, inv, sa, sb, target);
      else if (mix == kRiseToWet) stage3Moving<true, kRiseToWet>(in, out, n, a, b, inv, sa, sb, target);
      else stage3Moving<true, kFollowDrive>(in, out, n, a, b, inv, sa, sb, target);
    } else {
      if (mix == kWetOnly) stage3Moving<false, kWetOnly>(in, out, n, a, b, inv, sa, sb, target);
      else if (mix == kRiseToWet) stage3Moving<false, kRiseToWet>(in, out, n, a, b, inv, sa, sb, target);
      else stage3Moving<false, kFollowDrive>(in, out, n, a, b, inv, sa, sb, target);
    }
    r1_.carry(n);
    r2_.carry(n);

    drive_ = driveEnd;
    tone_ = toneEnd;
    shelf_ = sb;
    shelfTone_ = toneEnd;
    gainCache_ = b;
    gainDrive_ = driveEnd;
  }

  // Every filter state the rails do not cut off is finite. A NaN or infinity in the input lands in the
  // stage 1 filters and the final DC blocker first.
  bool stateFinite() const { return std::isfinite(chain_.s1Lp.y) && std::isfinite(chain_.dc.y); }

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
  Shelf shelf_{1.0f, 0.0f, 0.0f, 0.0f, 0.0f};  // the treble cut at TONE shelfTone_
  float shelfTone_ = -1.0f;
  Rail rail_{{0.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
  Gains gainCache_{1.0f, 1.0f, 1.0f};
  float gainDrive_ = -1.0f;  // the DRIVE gainCache_ was computed at
  float stackLoss_ = 1.0f, trim_ = 1.0f, g1Min_ = 1.0f, g2Min_ = 1.0f;
  float log2R1_ = 0.0f, log2R2_ = 0.0f, fadeSpan_ = 1.0f;  // the stage gains' octaves over DRIVE 0 to 1
  float boostFrom_ = 1.0f, boostK_ = 0.0f;  // top boost: start DRIVE, octaves per unit DRIVE past it
  float shelfCos_ = 1.0f, shelfAlpha_ = 0.0f;  // the treble cut's corner: cos(w0) and sin(w0) / sqrt(2)
};

}  // namespace cv
