#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"

// Monophonic pitch tracker. Two-stage YIN: a coarse search on a 12 kHz copy
// finds the period cheaply, then a narrow search at 48 kHz refines it when the
// caller asks for the fine pass (Autotune at every RESPONSE and the Formant card
// need it; the Mob card does not).
//
// A frame needs a full 21 ms of the new note before it reads voiced, so at a note that starts
// mid-phrase the period would hold the old note for 25 to 145 ms and then step. While the tracker
// reads unvoiced and the newest 20 ms are above the level floor, a cheap onset estimate on those 20 ms
// takes the period as soon as it is clear. When the tracker voices, the period slides from the
// estimate to the tracker's reading over kSlideSamples. The first note after a reset has no period
// to step from, so it waits for the tracker.

namespace cv {

struct PitchResult {
  // Samples at kSampleRate; 0 until the first voiced frame or onset estimate and while the tracker is idle.
  // While unvoiced it holds the last voiced period, or the onset estimate once one is clear, and for
  // PitchTracker::kSlideSamples after the tracker voices it slides from the estimate to its reading.
  float period = 0.0f;
  float hz = 0.0f;
  float rawHz = 0.0f;    // newest frame before the median; one hop fresher than hz
  bool voiced = false;

  // The newest voiced frame repeats faster than kMaxHz, so the period read is a multiple of the
  // note's, or its dip runs off the slow end of the search. The period stays as read.
  bool outOfRange = false;

  // The newest voiced frame's period sits on the edge of the search: the fine pass's minimum on
  // kMinPeriod or kMaxPeriod, or the coarse pass's pick at its slow end. The note lies at or past
  // that edge, so the period read is the edge, not the note.
  bool clamped = false;
};

class PitchTracker {
 public:
  static constexpr int kHop = 256;
  static constexpr float kMinHz = 70.0f;
  static constexpr float kMaxHz = 1000.0f;
  static constexpr float kVoicedThreshold = 0.15f;  // YIN threshold on the coarse search

  // The periods the tracker reports, in samples at kSampleRate: kMaxHz and kMinHz rounded outward to
  // a whole sample (static_asserts below). The PSOLA voices read these bounds.
  static constexpr int kMinPeriod = 48;
  static constexpr int kMaxPeriod = 686;

  // The period a voice plays at: the one given, held to the tracker's bounds.
  static float clampPeriod(float period) {
    return clampf(period, static_cast<float>(kMinPeriod), static_cast<float>(kMaxPeriod));
  }

  // A frame rebuilt on enable takes this many block sums per block on top of the block's own, so
  // no single block carries the whole rebuild.
  static constexpr int kRebuildSumsPerBlock = 2;

  // The onset estimate. It reads the newest 20 ms (kOnsetBlocks blocks) when their rms is above kMinRms,
  // and it counts as a pitch when its clarity, one minus the normalised difference at the picked lag,
  // reaches kOnsetClarity. Breath and consonants stay under it. The period slides from the estimate to
  // the tracker's reading over kSlideSamples, 15 ms.
  static constexpr float kOnsetClarity = 0.90f;
  static constexpr int kOnsetBlocks = 15;
  static constexpr int kSlideSamples = 720;

  // The caller's ring of input. The fine pass reads its frame from it, and after an idle spell the
  // tracker replays its history from it. The caller writes a call into the ring before the push, so
  // the ring holds the call's samples from head on.
  struct History {
    const float* ring;
    int len;
    int head;  // the ring index the call's first sample takes
  };

  PitchTracker() {
    lp_.setLowPass(kLowpassHz, kButterworthQ);
    reset();
  }

  void reset() {
    zeroFloats(dec_.data(), dec_.size());
    decPos_ = 0;
    zeroFloats(hopSums_.data(), hopSums_.size());
    hopRow_ = 0;
    zeroFloats(numer_.data(), numer_.size());
    denom_.fill(1.0f);
    tail_ = false;
    staleFrom_ = kFrameBlocks;
    sumsThisPush_ = 0;
    hopCount_ = 0;
    levels_.fill(0.0f);
    levelRow_ = 0;
    energy_ = 0.0f;
    lp_.clear();
    periods_.fill(0.0f);
    nPeriods_ = 0;
    nextPeriod_ = 0;
    active_ = false;
    result_ = PitchResult{};
    rested_ = false;
    behind_ = 0;
    untilHop_ = 0;
    warm_ = 0;
    clearOnset();
  }

  // Feed any n samples, which h's ring already holds. The decimated ring fills on every push, so the
  // frame is warm only while the stage keeps running; the chain resets PitchFx when every pitch card
  // is off, and the first voiced frame after such a switch-on comes about 32 ms in, where a running
  // stage gives it in 8 ms. Analysis runs every kHop samples while active and the block sums are
  // current. An enable rebuilds the block sums over the next few blocks and analyses at the end of
  // the block that makes them whole, unless a hop already did in that block. Inactive, the result
  // reads unvoiced. fine runs the 48 kHz fine pass on the coarse lag; without it an analysis
  // publishes the coarse lag times kDecim. A change takes effect at the next analysis, with nothing
  // to rebuild.
  //
  // After an idle spell an active push first replays the history from the ring. The low-pass restarts
  // kReplaySpan samples before the first hop at least kReplayBlocks blocks ahead, and the replay's
  // block sums come on where that hop's frame starts. The replay catches up in even shares one block
  // before that hop, so the hop's block, which runs the frame's first analysis, carries no share of it.
  void push(const float* in, int n, bool active, bool fine, const History& h) {
    ring_ = h.ring;
    ringLen_ = h.len;
    ringNext_ = h.head;
    sumsThisPush_ = 0;
    refineLagsThisPush_ = 0;
    fine_ = fine;
    if (active && rested_) startReplay();
    if (active && untilHop_ > 0) {
      replay(in, n, h);
      return;
    }
    if (active != active_) setActive(active);
    const bool stale = active_ && staleFrom_ < kFrameBlocks;
    if (stale) rebuildSome(n);
    const bool analysed = feed(in, n);
    if (stale && staleFrom_ == kFrameBlocks && !analysed) analyse();
    advanceSlide(n);
  }

  // Whether an analysis works out PitchResult::outOfRange. It costs ten compares a hop, so only the
  // callers that read the flag (the Mob and Formant cards) ask for it. It starts on.
  void readOutOfRange(bool on) { rangeFlag_ = on; }

  // n samples with no reader. The hop count keeps time, the rings rest and the result reads
  // unvoiced; the next push with a History replays them.
  void idle(int n) {
    if (active_) setActive(false);
    hopCount_ = (hopCount_ + behind_ + n) % kHop;
    behind_ = 0;
    untilHop_ = 0;
    rested_ = true;
  }

  const PitchResult& result() const { return result_; }

  // Test hooks.
  static constexpr int kCoarseWForTest = 256;
  static constexpr int kCoarseMinForTest = 12;
  static constexpr int kCoarseMaxForTest = 171;
  static constexpr int kFineWForTest = 512;
  static constexpr int kFineSpanForTest = 1198;
  static constexpr int kDecimForTest = 4;
  const float* coarseFrameForTest() const { return coarseFrame(); }
  float lastCoarseTau() const { return lastTau_; }
  bool lastCoarsePeriodic() const { return lastPeriodic_; }
  bool coarseRanForTest() const { return coarseRan_; }
  int sumsLastPushForTest() const { return sumsThisPush_; }
  bool sumsCurrentForTest() const { return staleFrom_ == kFrameBlocks; }
  uint32_t onsetRunsForTest() const { return onsetRuns_; }
  float onsetBestDnForTest() const { return onsetBestDn_; }  // the last run's lowest normalised difference
  int refineLagsLastPushForTest() const { return refineLagsThisPush_; }
  static constexpr int onsetMaxLagsForTest() { return kOnsetMaxLags; }

 private:
  static constexpr int kDecLen = 512;
  static constexpr int kDecim = 4;
  static constexpr int kCoarseW = 256;
  // The coarse lags, in decimated samples: kMaxHz and kMinHz rounded inward (static_asserts below).
  static constexpr int kCoarseMin = 12;
  static constexpr int kCoarseMax = 171;
  static constexpr int kCoarseSpan = kCoarseW + kCoarseMax;

  // The coarse pass sums, per lag, the squared differences over the frame. The frame is kFrameBlocks
  // blocks of the decimated stream, kDecBlock samples each. A block sum is those sums over one block,
  // a hop sum adds the block sums of one hop, and the frame's sum adds its kHopSums hop sums.
  static constexpr int kDecBlock = kBlock / kDecim;
  static constexpr int kFrameBlocks = kCoarseW / kDecBlock;
  static constexpr int kBlocksPerHop = kHop / kBlock;
  static constexpr int kHopSums = kFrameBlocks / kBlocksPerHop;  // the coarse frame's hops
  static constexpr int kRowLen = kCoarseMax + 1;                  // per-lag sums, index 0 unused
  // The fine pass compares the kFineW samples that end kMaxPeriod before the newest with a copy up
  // to kMaxPeriod newer, so the frame spans kFineSpan.
  static constexpr int kFineW = 512;
  static constexpr int kFineReach = 4;
  static constexpr int kFineSpan = kFineW + kMaxPeriod;
  static constexpr float kMinRms = 0.0031623f;  // -50 dBFS
  static constexpr int kLevelW = 1024;           // the level gate's window, newest samples
  static constexpr int kLevelBlocks = kLevelW / kBlock;
  static constexpr float kLowpassHz = 2500.0f;

  // The onset estimate halves the decimated ring once more, to 6 kHz. Its window is the newest kOnsetW
  // samples, 20 ms, and its lags run from kMaxHz to 100 Hz. The difference function integrates over the
  // newest kOnsetL samples, so every lag sees the same number of terms.
  static constexpr int kOnsetHalve = 2;
  static constexpr int kOnsetW = 120;
  static constexpr int kOnsetMinLag = 6;
  static constexpr int kOnsetMaxLag = 60;
  static constexpr int kOnsetL = kOnsetW - kOnsetMaxLag;
  static constexpr int kOnsetFineW = kOnsetL * kOnsetHalve * kDecim;  // the refinement's window at 48 kHz
  static constexpr int kOnsetReach = 6;
  static constexpr float kOnsetNoiseDn = 0.5f;
  static constexpr int kOnsetSkipBlocks = 3;
  // The refinement sums this many lags at most (of the 2 * kOnsetReach + 1 it could): the centre and its
  // neighbours, and up to five more down the slope.
  static constexpr int kOnsetMaxLags = 8;

 public:
  // The history a replay reads: the coarse frame's span at full rate, or the fine frame's if longer.
  static constexpr int kReplaySpan = kFineSpan > kDecim * kCoarseSpan ? kFineSpan : kDecim * kCoarseSpan;

 private:
  // A replay's first analysis runs at the first hop at least this many blocks ahead. Spread over
  // eight blocks, its kFrameBlocks block sums and kReplaySpan low-pass samples come to at most three
  // block sums a block, as a rebuild's do, and each block costs less than a voiced hop block on the
  // host bench.
  static constexpr int kReplayBlocks = 8;
  // Samples of a replay before the first analysis's frame starts; the block sums stay off for them.
  static constexpr int kReplayWarm = kReplaySpan - kDecim * kCoarseW;
  // Past the warm-up a replayed sample also adds a block sum every block, which on the host costs
  // about as much again as its low-pass step, so the pace counts it twice.
  static constexpr int kSumWeight = 2;

  static_assert(static_cast<float>(kMinPeriod) == kSampleRate / kMaxHz &&
                    static_cast<float>(kMaxPeriod) >= kSampleRate / kMinHz &&
                    static_cast<float>(kMaxPeriod - 1) < kSampleRate / kMinHz,
                "the periods are the search range kMaxHz to kMinHz in samples, the slow end rounded up");
  static_assert(static_cast<float>(kCoarseMin) == static_cast<float>(kSampleRate / kDecim) / kMaxHz &&
                    static_cast<float>(kCoarseMax) <= static_cast<float>(kSampleRate / kDecim) / kMinHz &&
                    static_cast<float>(kCoarseMax + 1) > static_cast<float>(kSampleRate / kDecim) / kMinHz,
                "the coarse lags are the search range in decimated samples, the slow end rounded down");
  static_assert(kCoarseW % 4 == 0 && kFineW % 4 == 0, "windows must unroll by four");
  static_assert(kCoarseMax % 2 == 1, "the paired lag loop leaves one lag for the tail");
  static_assert(kCoarseSpan <= kDecLen, "coarse frame exceeds decimated ring");
  static_assert(kBlock % kDecim == 0, "a block is a whole number of decimated samples");
  static_assert(kCoarseW % kDecBlock == 0, "the coarse window is a whole number of blocks");
  static_assert(kHop % (kDecBlock * kDecim) == 0, "hops land on block boundaries");
  static_assert(kFrameBlocks % kBlocksPerHop == 0, "the coarse frame is a whole number of hops");
  static_assert(kCoarseSpan + kDecBlock <= kDecLen, "block sum reach exceeds decimated ring");
  static_assert(kDecBlock % 4 == 0, "blocks must unroll by four");
  static_assert(kCoarseWForTest == kCoarseW && kCoarseMinForTest == kCoarseMin &&
                    kCoarseMaxForTest == kCoarseMax,
                "test hooks mirror the coarse constants");
  static_assert(kFineWForTest == kFineW && kFineSpanForTest == kFineSpan && kDecimForTest == kDecim,
                "test hooks mirror the fine and decimation constants");
  static_assert(kHop % kBlock == 0 && kLevelW % kBlock == 0, "level blocks tile the hop and the window");
  static_assert(kRebuildSumsPerBlock >= 1, "an enable must make progress on its rebuild");
  static_assert(kReplaySpan >= kLevelW && kReplayWarm >= 0, "a replay covers every window the analysis reads");
  static_assert(kReplaySpan > kReplayBlocks * kBlock + kHop, "a replay always starts in the caller's ring");
  static_assert(kOnsetBlocks * kBlock == kOnsetHalve * kDecim * kOnsetW && kOnsetBlocks < kLevelBlocks,
                "the onset window is whole blocks, fewer than the level gate's");
  static_assert(kOnsetHalve * kOnsetW <= kDecLen && kOnsetL % 4 == 0 && kOnsetL > 0,
                "the onset window sits in the decimated ring and its sums unroll by four");
  static_assert(kOnsetFineW % 4 == 0 && kOnsetFineW + kMaxPeriod <= kFineSpan,
                "the refinement unrolls by four and reads no further back than the fine pass does");
  static_assert(kOnsetMaxLags >= 3 && kOnsetMaxLags <= 2 * kOnsetReach + 1,
                "the refinement sums the centre lag and both neighbours, and no more lags than it covers");
  static_assert(kOnsetMinLag * kOnsetHalve * kDecim == kMinPeriod && kOnsetMaxLag * kOnsetHalve * kDecim <= kMaxPeriod,
                "the onset lags run from kMaxHz down");
  static_assert(kDecim * kCoarseW % kHop == 0, "the coarse frame is whole hops, so a replay's block sums start on one");

  // Vertex offset of the parabola through three points, 0 if not a minimum.
  static float parabolic(float a, float b, float c) {
    const float denom = a - 2.0f * b + c;
    if (denom <= 0.0f) return 0.0f;
    const float off = 0.5f * (a - c) / denom;
    return off < -1.0f ? -1.0f : (off > 1.0f ? 1.0f : off);
  }

  // The decimated ring is written twice, at pos and pos + len, so the newest span is always
  // contiguous in the upper copy and the analysis reads it in place.
  const float* coarseFrame() const { return dec_.data() + decPos_ + kDecLen - kCoarseSpan; }

  // Sum of (a[j] - b[j])^2 over n samples, n a multiple of 4. Four accumulators
  // keep the FPU pipeline full; one accumulator serialises on its own latency.
  static float sumSqDiff(const float* a, const float* b, int n) {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    for (int j = 0; j < n; j += 4) {
      const float e0 = a[j] - b[j];
      const float e1 = a[j + 1] - b[j + 1];
      const float e2 = a[j + 2] - b[j + 2];
      const float e3 = a[j + 3] - b[j + 3];
      s0 += e0 * e0;
      s1 += e1 * e1;
      s2 += e2 * e2;
      s3 += e3 * e3;
    }
    return (s0 + s1) + (s2 + s3);
  }

  // sumSqDiff over a ring of len samples: the n samples from index ia against the n from index ib,
  // each wrapping at len, n a multiple of 4. It keeps the four accumulators and their order, so it
  // equals sumSqDiff on the unwrapped samples bit for bit. Stretches that touch no seam run as
  // sumSqDiff does, and the one group of four that straddles a seam runs sample by sample.
  static float sumSqDiffRing(const float* r, int len, int ia, int ib, int n) {
    if (ia + n <= len && ib + n <= len) return sumSqDiff(r + ia, r + ib, n);
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    for (int j = 0; j < n;) {
      const int room = len - (ia > ib ? ia : ib);  // samples before the nearer seam
      const int run = (n - j < room ? n - j : room) & ~3;
      if (run > 0) {
        const float* a = r + ia;
        const float* b = r + ib;
        for (int k = 0; k < run; k += 4) {
          const float e0 = a[k] - b[k];
          const float e1 = a[k + 1] - b[k + 1];
          const float e2 = a[k + 2] - b[k + 2];
          const float e3 = a[k + 3] - b[k + 3];
          s0 += e0 * e0;
          s1 += e1 * e1;
          s2 += e2 * e2;
          s3 += e3 * e3;
        }
        j += run;
        ia = ia + run == len ? 0 : ia + run;
        ib = ib + run == len ? 0 : ib + run;
        continue;
      }
      const int a1 = ia + 1 < len ? ia + 1 : ia + 1 - len, a2 = a1 + 1 < len ? a1 + 1 : a1 + 1 - len,
                a3 = a2 + 1 < len ? a2 + 1 : a2 + 1 - len;
      const int b1 = ib + 1 < len ? ib + 1 : ib + 1 - len, b2 = b1 + 1 < len ? b1 + 1 : b1 + 1 - len,
                b3 = b2 + 1 < len ? b2 + 1 : b2 + 1 - len;
      const float e0 = r[ia] - r[ib];
      const float e1 = r[a1] - r[b1];
      const float e2 = r[a2] - r[b2];
      const float e3 = r[a3] - r[b3];
      s0 += e0 * e0;
      s1 += e1 * e1;
      s2 += e2 * e2;
      s3 += e3 * e3;
      j += 4;
      ia = a3 + 1 < len ? a3 + 1 : a3 + 1 - len;
      ib = b3 + 1 < len ? b3 + 1 : b3 + 1 - len;
    }
    return (s0 + s1) + (s2 + s3);
  }

  // The same for two adjacent lags at once, b and b + 1: the M7 pairs one load
  // with one float op per cycle, so sharing the loads nearly halves the loop.
  static void sumSqDiff2(const float* a, const float* b, int n, float& d0, float& d1) {
    float s0 = 0.0f, s1 = 0.0f, t0 = 0.0f, t1 = 0.0f;
    for (int j = 0; j < n; j += 4) {
      const float a0 = a[j], a1 = a[j + 1], a2 = a[j + 2], a3 = a[j + 3];
      const float b0 = b[j], b1 = b[j + 1], b2 = b[j + 2], b3 = b[j + 3], b4 = b[j + 4];
      const float e0 = a0 - b0, e1 = a1 - b1, e2 = a2 - b2, e3 = a3 - b3;
      const float f0 = a0 - b1, f1 = a1 - b2, f2 = a2 - b3, f3 = a3 - b4;
      s0 += e0 * e0;
      s1 += e1 * e1;
      s0 += e2 * e2;
      s1 += e3 * e3;
      t0 += f0 * f0;
      t1 += f1 * f1;
      t0 += f2 * f2;
      t1 += f3 * f3;
    }
    d0 = s0 + s1;
    d1 = t0 + t1;
  }

  // The level gate: the newest kLevelW samples, from the energy of each block, against kMinRms.
  bool loud() const {
    float energy = 0.0f;
    for (float e : levels_) energy += e;
    return energy > kMinRms * kMinRms * static_cast<float>(kLevelW);
  }

  // The onset estimate's level gate: the newest kOnsetBlocks blocks, from the energy of each, against kMinRms.
  // levelRow_ names the oldest block, which the window leaves out.
  bool onsetLoud() const {
    float energy = 0.0f;
    for (float e : levels_) energy += e;
    energy -= levels_[static_cast<size_t>(levelRow_)];
    return energy > kMinRms * kMinRms * static_cast<float>(kOnsetBlocks * kBlock);
  }

  static float dot(const float* a, const float* b, int n) {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    for (int j = 0; j < n; j += 4) {
      s0 += a[j] * b[j];
      s1 += a[j + 1] * b[j + 1];
      s2 += a[j + 2] * b[j + 2];
      s3 += a[j + 3] * b[j + 3];
    }
    return (s0 + s1) + (s2 + s3);
  }

  // The coarse half of an estimate: YIN on the newest 20 ms at 6 kHz. The difference at lag t is
  // e0 + e(t) - 2 c(t), with c the correlation of the newest kOnsetL samples with the ones t back and e0
  // and e(t) their energies. e(t) follows from e(t - 1) by one sample in and one out. The normalised
  // difference divides by the mean of the differences up to t, as YIN does, so a smooth noise has no dip
  // at a short lag. The pick is the first lag from kOnsetMinLag (kMaxHz) up whose value falls under
  // 1 - kOnsetClarity, walked down to its bottom. The loop keeps the values around the pick, so the
  // parabola needs no array of them. Returns the coarse period in samples at kSampleRate, or 0 when no
  // dip is clear.
  float onsetPeriod() {
    ++onsetRuns_;
    const float* src = dec_.data() + decPos_ + kDecLen - kOnsetHalve * kOnsetW;
    std::array<float, kOnsetW> y;
    float e0 = 0.0f;
    for (int i = 0; i < kOnsetW; ++i) {
      const float v = 0.5f * (src[kOnsetHalve * i] + src[kOnsetHalve * i + 1]);
      y[static_cast<size_t>(i)] = v;
      if (i >= kOnsetW - kOnsetL) e0 += v * v;
    }
    onsetBestDn_ = 1.0f;
    if (e0 <= kMinRms * kMinRms * static_cast<float>(kOnsetL)) return 0.0f;
    const float* cur = y.data() + (kOnsetW - kOnsetL);
    const float dip = 1.0f - kOnsetClarity;
    float et = e0;
    float running = 0.0f;
    float prev = 1.0f, before = 1.0f, pickDn = 1.0f, after = 1.0f, best = 1.0f;
    int pick = 0;
    for (int t = 1; t <= kOnsetMaxLag; ++t) {
      const float out = y[static_cast<size_t>(kOnsetW - t)], in = y[static_cast<size_t>(kOnsetW - kOnsetL - t)];
      et += in * in - out * out;
      const float d = fmaxf(e0 + et - 2.0f * dot(cur, cur - t, kOnsetL), 0.0f);
      running += d;
      const float dn = running > 0.0f ? d * static_cast<float>(t) / running : 1.0f;
      if (t >= kOnsetMinLag) best = fminf(best, dn);
      if (pick == 0) {
        if (t >= kOnsetMinLag && dn < dip) {
          pick = t;
          pickDn = dn;
          before = prev;
        }
      } else if (dn < pickDn) {
        pick = t;
        pickDn = dn;
        before = prev;
      } else {
        after = dn;
        break;
      }
      prev = dn;
    }
    if (pick == 0) {
      onsetBestDn_ = best;
      return 0.0f;
    }
    onsetBestDn_ = pickDn;
    const float off = pick < kOnsetMaxLag ? parabolic(before, pickDn, after) : 0.0f;
    return clampf((static_cast<float>(pick) + off) * static_cast<float>(kOnsetHalve * kDecim),
                  static_cast<float>(kMinPeriod), static_cast<float>(kMaxPeriod));
  }

  // The estimate refined at 48 kHz on the same 20 ms: the plain difference of the newest kOnsetFineW
  // samples against the ones lag older, at the lags around the coarse period, and a parabola through the
  // lowest. It finds the lowest as the fine pass does: the sums at the centre lag and its two neighbours,
  // then one lag at a time downhill while the sum keeps falling, each lag summed once. Summing all
  // 2 * kOnsetReach + 1 lags took 1.6 host us, more than a voiced hop's whole analysis, in the block of the
  // coarse half. The walk sums three or four lags on Adam's clips and at most kOnsetMaxLags. A walk that
  // would need more keeps the coarse period, as one that ends on an end of the search does.
  float refineOnset(float coarse) {
    const int centre = static_cast<int>(floorf(coarse + 0.5f));
    const int lo = centre - kOnsetReach < kMinPeriod ? kMinPeriod : centre - kOnsetReach;
    const int hi = centre + kOnsetReach > kMaxPeriod ? kMaxPeriod : centre + kOnsetReach;
    const int start = ringNext_ >= kOnsetFineW ? ringNext_ - kOnsetFineW : ringNext_ - kOnsetFineW + ringLen_;
    std::array<float, 2 * kOnsetReach + 1> d{};
    int summed = 0;
    auto sum = [&](int lag) {
      const int older = start >= lag ? start - lag : start - lag + ringLen_;
      ++summed;
      return d[static_cast<size_t>(lag - lo)] = sumSqDiffRing(ring_, ringLen_, start, older, kOnsetFineW);
    };
    int m = centre < lo ? lo : (centre > hi ? hi : centre);
    const float dm = sum(m);
    const float dl = m > lo ? sum(m - 1) : dm;
    const float dr = m < hi ? sum(m + 1) : dm;
    bool capped = false;
    if (m > lo && dl <= dm && dl <= dr) {
      for (--m; m > lo; --m) {
        capped = summed == kOnsetMaxLags;
        if (capped || sum(m - 1) > d[static_cast<size_t>(m - lo)]) break;
      }
    } else if (dr < dm) {
      for (++m; m < hi; ++m) {
        capped = summed == kOnsetMaxLags;
        if (capped || sum(m + 1) >= d[static_cast<size_t>(m - lo)]) break;
      }
    }
    refineLagsThisPush_ += summed;
    if (capped || m == lo || m == hi) return coarse;
    const size_t k = static_cast<size_t>(m - lo);
    return static_cast<float>(m) + parabolic(d[k - 1], d[k], d[k + 1]);
  }

  // Once a block while the tracker reads unvoiced. A quiet window arms the estimate, and the first clear
  // estimate after that becomes the held period. The period keeps it until the tracker voices. With no
  // period held yet (the first note after a reset) there is no old note to step from, so the tracker
  // alone starts the cards. A run whose lowest difference stays above kOnsetNoiseDn is noise, so the next
  // kOnsetSkipBlocks blocks skip the estimate, which holds a long breath or room noise to a quarter of the
  // runs.
  void estimateOnset() {
    if (!onsetLoud()) {
      onsetArmed_ = true;
      onsetWait_ = 0;
      return;
    }
    if (!onsetArmed_ || result_.period <= 0.0f) return;
    if (onsetWait_ > 0) {
      --onsetWait_;
      return;
    }
    const float coarse = onsetPeriod();
    if (coarse <= 0.0f) {
      if (onsetBestDn_ > kOnsetNoiseDn) onsetWait_ = kOnsetSkipBlocks;
      return;
    }
    const float p = refineOnset(coarse);
    onsetArmed_ = false;
    estimateHeld_ = true;
    slideLeft_ = 0;
    result_.period = p;
    result_.hz = kSampleRate / p;
  }

  // The tracker's period plus what is left of the slide from the estimate.
  float slidPeriod() const {
    return base_ + slideOffset_ * (static_cast<float>(slideLeft_) * (1.0f / static_cast<float>(kSlideSamples)));
  }

  void advanceSlide(int n) {
    if (slideLeft_ <= 0) return;
    slideLeft_ = slideLeft_ > n ? slideLeft_ - n : 0;
    result_.period = slidPeriod();
    result_.hz = kSampleRate / result_.period;
  }

  void clearOnset() {
    estimateHeld_ = false;
    onsetArmed_ = true;
    onsetWait_ = 0;
    slideLeft_ = 0;
    slideOffset_ = 0.0f;
    base_ = 0.0f;
  }

  // A frame under the level gate publishes nothing, so it skips the coarse pass. The enable's tail
  // is spent either way.
  void analyse() {
    float tau = 0.0f;
    coarseRan_ = loud();
    if (coarseRan_ && coarse(tau)) {
      publishVoiced(fine_ ? finePass(tau) : tau * static_cast<float>(kDecim));
      result_.outOfRange = lastOutOfRange_;
      result_.clamped = lastClamped_;
      return;
    }
    tail_ = false;
    result_.voiced = false;
    result_.outOfRange = false;
    result_.clamped = false;
  }

  // A dip under the threshold at a lag below kCoarseMin means the frame repeats faster than kMaxHz.
  // The coarse pass already holds those lags, so the test costs ten compares.
  bool dipsBelowRange() const {
    for (int t = 2; t < kCoarseMin; ++t) {
      if (numer_[t] < kVoicedThreshold * denom_[t]) return true;
    }
    return false;
  }

  // The coarse frame's sum at lag t: the hop sums added in order, and on an enable's first analysis
  // the tail, which numer_ holds until the pass overwrites it.
  float frameSumAt(int t, bool tail) const {
    float sum = hopSums_[static_cast<size_t>(t)];
    for (int r = 1; r < kHopSums; ++r) sum += hopSums_[static_cast<size_t>(r * kRowLen + t)];
    return tail ? sum + numer_[static_cast<size_t>(t)] : sum;
  }

  // One pass over the lags of the 12 kHz frame. The normalised difference of YIN at lag t is the
  // frame sum there times t over the running sum of the sums up to it. The pass keeps it as the
  // fraction numer_[t] / denom_[t], and finds the lowest value from kCoarseMin up (best) and the first
  // dip under kVoicedThreshold (first, -1 for none) by cross-multiplying, so nothing divides. A
  // frame of exact zeros has no running sum and reads 1, no dip.
  void scanLags(int& best, int& first) {
    const bool tail = tail_;
    tail_ = false;
    float running = 0.0f;
    float bestN = 0.0f, bestD = 1.0f;
    best = kCoarseMin;
    first = -1;
    for (int t = 1; t <= kCoarseMax; ++t) {
      const float sum = frameSumAt(t, tail);
      running += sum;
      const float n = running > 0.0f ? sum * static_cast<float>(t) : 1.0f;
      const float d = running > 0.0f ? running : 1.0f;
      numer_[static_cast<size_t>(t)] = n;
      denom_[static_cast<size_t>(t)] = d;
      if (t < kCoarseMin) continue;
      if (t == kCoarseMin || n * bestD < bestN * d) {
        best = t;
        bestN = n;
        bestD = d;
      }
      if (first < 0 && n < kVoicedThreshold * d) first = t;
    }
    lastPeriodic_ = bestN < kVoicedThreshold * bestD;
  }

  // YIN on the 12 kHz frame. Returns whether min d' is below kVoicedThreshold and writes the
  // interpolated candidate lag in decimated samples. The pick is the first dip, walked down to its
  // bottom, or else the lowest value. Only the parabola's three lags divide.
  bool coarse(float& tau) {
    int best, first;
    scanLags(best, first);
    if (first >= 0) {
      while (first < kCoarseMax && numer_[first + 1] * denom_[first] < numer_[first] * denom_[first + 1]) ++first;
    }
    const int pick = first >= 0 ? first : best;
    auto at = [this](int t) { return numer_[static_cast<size_t>(t)] / denom_[static_cast<size_t>(t)]; };
    const float off = pick < kCoarseMax ? parabolic(at(pick - 1), at(pick), at(pick + 1)) : 0.0f;
    tau = static_cast<float>(pick) + off;
    lastTau_ = tau;
    lastClamped_ = pick == kCoarseMax;
    lastOutOfRange_ = rangeFlag_ && (lastClamped_ || dipsBelowRange());
    return lastPeriodic_;
  }

  // Adds the per-lag squared differences over one block, whose first sample is x[0], into row. Lags
  // go in pairs so each pass shares its loads; kCoarseMax is odd, so the last lag runs alone.
  static void blockSumKernel(const float* x, float* row) {
    for (int t = 1; t < kCoarseMax; t += 2) {
      float a = 0.0f, b = 0.0f;
      sumSqDiff2(x, x + t, kDecBlock, a, b);
      row[t] += a;
      row[t + 1] += b;
    }
    row[kCoarseMax] += sumSqDiff(x, x + kCoarseMax, kDecBlock);
  }

  // Runs n samples through the rings in stretches that end on the hop's block edges. Each edge closes
  // a block; each hop analyses while active and whole. Returns whether a hop analysed.
  bool feed(const float* in, int n) {
    bool analysed = false;
    for (int i = 0; i < n;) {
      const int toEdge = kBlock - hopCount_ % kBlock;
      const int m = n - i < toEdge ? n - i : toEdge;
      fill(in + i, m);
      i += m;
      hopCount_ += m;
      if (hopCount_ % kBlock != 0) continue;
      closeBlock();
      if (hopCount_ < kHop) continue;
      hopCount_ = 0;
      if (active_ && staleFrom_ == kFrameBlocks) {
        analyse();
        analysed = true;
      }
      hopRow_ = hopRow_ + 1 == kHopSums ? 0 : hopRow_ + 1;
    }
    return analysed;
  }

  // The rings missed an idle spell. The replay will catch up at the first hop at least
  // kReplayBlocks blocks ahead; the hop count steps back to where the low-pass restarts.
  void startReplay() {
    int untilHop = kHop - hopCount_;
    while (untilHop < kReplayBlocks * kBlock) untilHop += kHop;
    untilHop_ = untilHop;
    behind_ = kReplaySpan - untilHop;
    hopCount_ = ((hopCount_ - behind_) % kHop + kHop) % kHop;
    warm_ = kReplayWarm;
    lp_.clear();
    energy_ = 0.0f;
    rested_ = false;
  }

  // This call's share of the replay, oldest first: the samples the rings missed come from the
  // caller's ring, then this call's own. The replay catches up one block before the hop; once
  // caught up, a call runs whole, with no rebuild, until that hop.
  void replay(const float* in, int n, const History& h) {
    const int ahead = untilHop_ - kBlock;  // the caller's samples left until the replay catches up
    const int share = ahead <= n ? behind_ + n : paceShare(n, ahead);
    const int fromRing = share < behind_ ? share : behind_;
    for (int done = 0; done < fromRing;) {
      int at = h.head - behind_ + done;
      if (at < 0) at += h.len;
      const int m = fromRing - done < h.len - at ? fromRing - done : h.len - at;
      ringNext_ = at;
      replaySpan(h.ring + at, m);
      done += m;
    }
    ringNext_ = h.head;
    replaySpan(in, share - fromRing);
    behind_ += n - share;
    untilHop_ -= n;
  }

  // An even share of the work left before the replay catches up, ahead samples from now. A sample
  // past the warm-up counts kSumWeight times, since those add a block sum every block.
  int paceShare(int n, int ahead) const {
    const int left = behind_ + ahead;
    const int work = warm_ + kSumWeight * (left - warm_);
    const int units = (work * n + ahead - 1) / ahead;
    const int share = units <= warm_ ? units : warm_ + (units - warm_ + kSumWeight - 1) / kSumWeight;
    return share < behind_ + n ? share : behind_ + n;
  }

  // m replayed samples. The block sums come on where the first analysis's frame starts.
  void replaySpan(const float* x, int m) {
    if (warm_ > 0) {
      const int k = warm_ < m ? warm_ : m;
      feed(x, k);
      x += k;
      m -= k;
      warm_ -= k;
      if (warm_ == 0) setActive(true);
    }
    feed(x, m);
  }

  // m samples that end at or before the next block edge of the hop, which sit in the caller's ring
  // from ringNext_ on. The low-pass runs on a local copy, since stores into the decimated ring could
  // alias its members and force a reload every sample, and every kDecim-th output goes to that ring.
  // The samples' squares add to the block's energy.
  void fill(const float* in, int m) {
    ringNext_ = ringNext_ + m < ringLen_ ? ringNext_ + m : ringNext_ + m - ringLen_;
    Biquad lp = lp_;
    int phase = hopCount_ % kDecim;
    float energy = energy_;
    for (int k = 0; k < m; ++k) {
      energy += in[k] * in[k];
      const float y = lp.run(in[k]);
      if (++phase == kDecim) {
        phase = 0;
        pushDecimated(y);
      }
    }
    lp_ = lp;
    energy_ = energy;
  }

  void pushDecimated(float y) {
    dec_[decPos_] = y;
    dec_[decPos_ + kDecLen] = y;
    decPos_ = (decPos_ + 1) & (kDecLen - 1);
  }

  // A block of the hop ends. Its energy joins the ring the level gate reads and, while active, its
  // block sum joins the hop's sum.
  void closeBlock() {
    levels_[static_cast<size_t>(levelRow_)] = energy_;
    levelRow_ = levelRow_ + 1 == kLevelBlocks ? 0 : levelRow_ + 1;
    energy_ = 0.0f;
    if (active_) addBlockSum();
    if (active_ && staleFrom_ == kFrameBlocks && !result_.voiced) estimateOnset();
  }

  // The block that just completed starts kDecBlock + kCoarseMax back. The hop's first block sum
  // takes over the hop sum of four hops back; before an enable's first analysis that sum's blocks are
  // the frame's oldest, so they move to the tail first.
  void addBlockSum() {
    float* row = hopSums_.data() + hopRow_ * kRowLen;
    if (hopCount_ == kBlock) {
      if (tail_) {
        for (int t = 1; t <= kCoarseMax; ++t) numer_[t] += row[t];
      }
      std::fill(row, row + kRowLen, 0.0f);
    }
    blockSumKernel(dec_.data() + decPos_ + kDecLen - (kDecBlock + kCoarseMax), row);
    // The new block sum replaces the oldest, so one stale block fewer is left in the frame.
    if (staleFrom_ < kFrameBlocks) ++staleFrom_;
    ++sumsThisPush_;
  }

  // A switch clears the result. Switching on zeroes the hop sums, which the next blocks rebuild,
  // and starts the tail empty.
  void setActive(bool active) {
    active_ = active;
    result_ = PitchResult{};
    clearOnset();
    staleFrom_ = active ? 0 : kFrameBlocks;
    if (!active) return;
    zeroFloats(hopSums_.data(), hopSums_.size());
    zeroFloats(numer_.data(), numer_.size());
    tail_ = true;
  }

  // Blocks of age staleFrom_ and older are missing from the sums. Rebuilds the newest of them, at
  // most kRebuildSumsPerBlock, and none that this call's own block sum would push out of the frame.
  void rebuildSome(int n) {
    const int limit = hopCount_ % kBlock + n >= kBlock ? kFrameBlocks - 1 : kFrameBlocks;
    for (int k = 0; k < kRebuildSumsPerBlock && staleFrom_ < limit; ++k) rebuildBlockSum(staleFrom_++);
  }

  // Age 0 is the newest complete block. Its sum starts kDecBlock + kCoarseMax back from the last
  // block boundary, and each older block's kDecBlock further. The open hop holds the newest blocks,
  // then each hop back four more; a block of the hop whose sum the open hop took over goes to the tail.
  void rebuildBlockSum(int age) {
    const int done = hopCount_ / kBlock;
    const int back = age < done ? 0 : 1 + (age - done) / kBlocksPerHop;
    const int open = (hopCount_ % kBlock) / kDecim;  // decimated samples into the open block
    const float* x = dec_.data() + decPos_ + kDecLen - open - (kDecBlock + kCoarseMax) - kDecBlock * age;
    const int row = (hopRow_ + kHopSums - back) % kHopSums;
    blockSumKernel(x, back < kHopSums ? hopSums_.data() + row * kRowLen : numer_.data());
    ++sumsThisPush_;
  }

  // The fine pass: the plain difference function at 48 kHz around kDecim * tauCoarse, kFineReach lags
  // each side. Inside the reach it has one dip, so the pass sums the centre lag and its two
  // neighbours, then walks one lag at a time toward the lower neighbour while the sum keeps falling,
  // never past lo or hi. A tie goes to the lower lag, as a scan up from lo would pick. Each lag is
  // summed at most once; the parabola reads the lowest sum and its two neighbours. A minimum on
  // kMinPeriod or kMaxPeriod marks the frame clamped.
  float finePass(float tauCoarse) {
    const int centre = static_cast<int>(floorf(tauCoarse * kDecim + 0.5f));
    const int lo = centre - kFineReach < kMinPeriod ? kMinPeriod : centre - kFineReach;
    const int hi = centre + kFineReach > kMaxPeriod ? kMaxPeriod : centre + kFineReach;
    // The frame is the newest kFineSpan samples of the caller's ring.
    const int start = ringNext_ >= kFineSpan ? ringNext_ - kFineSpan : ringNext_ - kFineSpan + ringLen_;
    std::array<float, 2 * kFineReach + 1> d{};
    auto sum = [&](int lag) {
      const int later = start + lag < ringLen_ ? start + lag : start + lag - ringLen_;
      return d[lag - lo] = sumSqDiffRing(ring_, ringLen_, start, later, kFineW);
    };
    int m = centre < lo ? lo : (centre > hi ? hi : centre);
    const float dm = sum(m);
    const float dl = m > lo ? sum(m - 1) : dm;
    const float dr = m < hi ? sum(m + 1) : dm;
    if (m > lo && dl <= dm && dl <= dr) {
      for (--m; m > lo && sum(m - 1) <= d[m - lo];) --m;
    } else if (dr < dm) {
      for (++m; m < hi && sum(m + 1) < d[m - lo];) ++m;
    }
    const int k = m - lo;
    const float off = (m > lo && m < hi) ? parabolic(d[k - 1], d[k], d[k + 1]) : 0.0f;
    if (m == kMinPeriod || m == kMaxPeriod) lastClamped_ = true;
    return static_cast<float>(m) + off;
  }

  // Median of the last three voiced periods suppresses single octave jumps. After an onset estimate the
  // median starts over, since the frames it holds belong to the old note, and the period slides from the
  // estimate to the new reading.
  void publishVoiced(float period) {
    if (estimateHeld_) {
      nPeriods_ = 0;
      nextPeriod_ = 0;
    }
    periods_[nextPeriod_] = period;
    nextPeriod_ = nextPeriod_ == 2 ? 0 : nextPeriod_ + 1;
    if (nPeriods_ < 3) ++nPeriods_;
    float p = period;
    if (nPeriods_ >= 3) {
      const float a = periods_[0], b = periods_[1], c = periods_[2];
      p = fmaxf(fminf(a, b), fminf(fmaxf(a, b), c));
    }
    if (estimateHeld_) {
      slideOffset_ = result_.period - p;
      slideLeft_ = kSlideSamples;
      estimateHeld_ = false;
    }
    base_ = p;
    const float shown = slidPeriod();
    result_.period = shown;
    result_.hz = kSampleRate / shown;
    result_.rawHz = kSampleRate / period;
    result_.voiced = true;
  }

  Biquad lp_;  // anti-alias low-pass ahead of the decimator
  const float* ring_ = nullptr;  // the caller's ring, set by each push
  int ringLen_ = 1;
  int ringNext_ = 0;                        // the ring index after the newest sample consumed
  std::array<float, 2 * kDecLen> dec_{};
  // The last coarse pass's normalised difference at each lag, as the fraction numer_ / denom_. From
  // an enable to its first analysis numer_ holds the tail instead: the frame's oldest block sums, of
  // the hop whose sum the open hop took over.
  std::array<float, kRowLen> numer_{};
  std::array<float, kRowLen> denom_{};
  bool tail_ = false;
  std::array<float, kHopSums * kRowLen> hopSums_{};  // per-lag sums of each hop's blocks
  int hopRow_ = 0;                                   // the open hop's sum; it moves on at each hop, active or not
  int staleFrom_ = kFrameBlocks;                     // blocks of this age and older still wait for their rebuild
  int sumsThisPush_ = 0;
  int refineLagsThisPush_ = 0;  // lags the onset refinement summed in this push
  float lastTau_ = 0.0f;
  bool lastPeriodic_ = false;
  bool lastOutOfRange_ = false;
  bool lastClamped_ = false;
  bool coarseRan_ = false;  // the last analysis passed the level gate and ran the coarse pass
  bool rangeFlag_ = true;   // analyses work out outOfRange
  std::array<float, 3> periods_{};
  int nPeriods_ = 0;
  int nextPeriod_ = 0;
  int decPos_ = 0;
  int hopCount_ = 0;  // samples into the open hop; the decimator's phase and the block's follow it
  std::array<float, kLevelBlocks> levels_{};  // energy of each block of the level gate's window
  int levelRow_ = 0;
  float energy_ = 0.0f;  // the open block's energy so far
  bool active_ = false;
  bool fine_ = true;
  PitchResult result_;

  // The onset estimate: whether the period holds an estimate, whether the next clear one may take it
  // (a quiet window arms it), the median's period as the tracker last gave it, what is left of the
  // slide from the estimate to it, and the estimates run so far.
  bool estimateHeld_ = false;
  bool onsetArmed_ = true;
  float base_ = 0.0f;
  float slideOffset_ = 0.0f;
  int slideLeft_ = 0;
  int onsetWait_ = 0;  // blocks left before the next run
  uint32_t onsetRuns_ = 0;  // for the tests; it wraps
  float onsetBestDn_ = 1.0f;

  bool rested_ = false;  // the rings missed an idle spell and replay at the next push with a History
  int behind_ = 0;       // samples a replay still runs behind the caller
  int untilHop_ = 0;     // the caller's samples left until a replay's first analysis
  int warm_ = 0;         // replayed samples left before the first analysis's frame starts
};

}  // namespace cv
