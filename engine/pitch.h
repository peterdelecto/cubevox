#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "engine/common.h"
#include "engine/dsp.h"

// Monophonic pitch tracker. Two-stage YIN: a coarse search on a 12 kHz copy
// finds the period cheaply, then a narrow search at 48 kHz refines it when the
// caller asks for the fine pass (Autotune's hard tune needs it; the Crowd card
// does not).

namespace cv {

struct PitchResult {
  float period = 0.0f;   // samples at kSampleRate; 0 until first voiced frame
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

  // The periods the tracker reports, in samples at kSampleRate. The PSOLA voices read these bounds.
  static constexpr int kMinPeriod = 48;
  static constexpr int kMaxPeriod = 686;

  // A frame rebuilt on enable takes this many panes per block on top of the block's own pane, so
  // no single block carries the whole rebuild.
  static constexpr int kRebuildPanesPerBlock = 2;

  // A caller's ring of past input. After an idle spell the tracker replays its history from it.
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
    full_.fill(0.0f);
    dec_.fill(0.0f);
    fullPos_ = 0;
    decPos_ = 0;
    hopSums_.fill(0.0f);
    hopRow_ = 0;
    cmnd_.fill(0.0f);
    tail_ = false;
    staleFrom_ = kPanes;
    panesThisPush_ = 0;
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
  }

  // Feed n <= kBlock samples. The rings always fill, so the frame is warm. Analysis runs every
  // kHop samples while active and the panes are current. An enable rebuilds the panes over the
  // next few blocks and analyses at the end of the block that makes them whole, unless a hop
  // already did in that block. Inactive, the result reads unvoiced. fine runs the 48 kHz refine on
  // the coarse lag; without it an analysis publishes the coarse lag times kDecim. A change takes
  // effect at the next analysis, with nothing to rebuild.
  void push(const float* in, int n, bool active = true, bool fine = true) {
    panesThisPush_ = 0;
    fine_ = fine;
    if (active != active_) setActive(active);
    const bool stale = active_ && staleFrom_ < kPanes;
    if (stale) rebuildSome(n);
    const bool analysed = feed(in, n);
    if (stale && staleFrom_ == kPanes && !analysed) analyse();
  }

  // Feed n <= kBlock samples while a reader is on, as push(in, n, true, fine). After an idle spell
  // the rings first replay from the caller's ring. The low-pass restarts kReplaySpan samples before
  // the first hop at least kReplayBlocks blocks ahead, and the replay's panes come on where that
  // hop's frame starts. The replay catches up in even shares one block before that hop, so the hop's
  // block, which runs the frame's first analysis, carries no share of it.
  void push(const float* in, int n, bool fine, const History& h) {
    if (rested_) startReplay();
    if (untilHop_ <= 0) {
      push(in, n, true, fine);
      return;
    }
    panesThisPush_ = 0;
    fine_ = fine;
    replay(in, n, h);
  }

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
  const float* coarseFrameForTest() const { return coarseFrame(); }
  float lastCoarseTau() const { return lastTau_; }
  bool lastCoarsePeriodic() const { return lastPeriodic_; }
  int panesLastPushForTest() const { return panesThisPush_; }
  bool panesCurrentForTest() const { return staleFrom_ == kPanes; }

 private:
  static constexpr int kFullLen = 2048;
  static constexpr int kDecLen = 512;
  static constexpr int kDecim = 4;
  static constexpr int kCoarseW = 256;
  static constexpr int kCoarseMin = 12;
  static constexpr int kCoarseMax = 171;
  static constexpr int kCoarseSpan = kCoarseW + kCoarseMax;
  static constexpr int kPaneLen = kBlock / kDecim;  // a pane spans one block of the hop
  static constexpr int kPanes = kCoarseW / kPaneLen;
  static constexpr int kPanesPerHop = kHop / kBlock;
  static constexpr int kHopSums = kPanes / kPanesPerHop;  // the coarse frame's hops
  static constexpr int kRowLen = kCoarseMax + 1;           // per-lag sums, index 0 unused
  // The fine pass compares the kFineW samples that end kMaxPeriod before the newest with a copy up
  // to kMaxPeriod newer, so the frame spans kFineSpan.
  static constexpr int kFineW = 512;
  static constexpr int kFineReach = 4;
  static constexpr int kFineSpan = kFineW + kMaxPeriod;
  static constexpr float kMinRms = 0.0031623f;  // -50 dBFS
  static constexpr int kLevelW = 1024;           // the level gate's window, newest samples
  static constexpr int kLevelBlocks = kLevelW / kBlock;
  static constexpr float kLowpassHz = 2500.0f;

 public:
  // The history a replay reads: the coarse frame's span at full rate, or the fine frame's if longer.
  static constexpr int kReplaySpan = kFineSpan > kDecim * kCoarseSpan ? kFineSpan : kDecim * kCoarseSpan;

 private:
  // A replay's first analysis runs at the first hop at least this many blocks ahead. Spread over
  // eight blocks, its kPanes panes and kReplaySpan low-pass samples come to at most three panes a
  // block, as a rebuild's do, and each block costs less than a voiced hop block on the host bench.
  static constexpr int kReplayBlocks = 8;
  // Samples of a replay before the first analysis's frame starts; the panes stay off for them.
  static constexpr int kReplayWarm = kReplaySpan - kDecim * kCoarseW;
  // Past the warm-up a replayed sample also adds a pane every block, which on the host costs about
  // as much again as its low-pass step, so the pace counts it twice.
  static constexpr int kPaneWeight = 2;

  static_assert(kCoarseW % 4 == 0 && kFineW % 4 == 0, "windows must unroll by four");
  static_assert(kCoarseMax % 2 == 1, "the paired lag loop leaves one lag for the tail");
  static_assert(kCoarseSpan <= kDecLen, "coarse frame exceeds decimated ring");
  static_assert(kBlock % kDecim == 0, "a block is a whole number of decimated samples");
  static_assert(kCoarseW % kPaneLen == 0, "the coarse window is a whole number of panes");
  static_assert(kHop % (kPaneLen * kDecim) == 0, "hops land on pane boundaries");
  static_assert(kPanes % kPanesPerHop == 0, "the coarse frame is a whole number of hops");
  static_assert(kCoarseSpan + kPaneLen <= kDecLen, "pane reach exceeds decimated ring");
  static_assert(kPaneLen % 4 == 0, "panes must unroll by four");
  static_assert(kCoarseWForTest == kCoarseW && kCoarseMinForTest == kCoarseMin &&
                    kCoarseMaxForTest == kCoarseMax,
                "test hooks mirror the coarse constants");
  static_assert(kFineWForTest == kFineW && kFineSpanForTest == kFineSpan, "test hooks mirror the fine constants");
  static_assert(kFineSpan <= kFullLen, "fine frame exceeds full-rate ring");
  static_assert(kHop % kBlock == 0 && kLevelW % kBlock == 0, "level blocks tile the hop and the window");
  static_assert(kRebuildPanesPerBlock >= 1, "an enable must make progress on its rebuild");
  static_assert(kReplaySpan >= kLevelW && kReplayWarm >= 0, "a replay covers every window the analysis reads");
  static_assert(kReplaySpan > kReplayBlocks * kBlock + kHop, "a replay always starts in the caller's ring");
  static_assert(kDecim * kCoarseW % kHop == 0, "the coarse frame is whole hops, so a replay's panes start on one");

  // Vertex offset of the parabola through three points, 0 if not a minimum.
  static float parabolic(float a, float b, float c) {
    const float denom = a - 2.0f * b + c;
    if (denom <= 0.0f) return 0.0f;
    const float off = 0.5f * (a - c) / denom;
    return off < -1.0f ? -1.0f : (off > 1.0f ? 1.0f : off);
  }

  // Each ring is written twice, at pos and pos + len, so the newest span is
  // always contiguous in the upper copy and the analysis reads it in place.
  const float* fineFrame() const { return full_.data() + fullPos_ + kFullLen - kFineSpan; }
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

  void analyse() {
    float tau = 0.0f;
    const bool periodic = coarse(tau);
    if (periodic && loud()) {
      publishVoiced(fine_ ? refine(tau) : tau * static_cast<float>(kDecim));
      result_.outOfRange = lastOutOfRange_;
      result_.clamped = lastClamped_;
    } else {
      result_.voiced = false;
      result_.outOfRange = false;
      result_.clamped = false;
    }
  }

  // A dip under the threshold at a lag below kCoarseMin means the frame repeats faster than kMaxHz.
  // The normalised difference already holds those lags, so the test costs ten compares.
  bool dipsBelowRange() const {
    for (int t = 2; t < kCoarseMin; ++t) {
      if (cmnd_[t] < kVoicedThreshold) return true;
    }
    return false;
  }

  // The coarse frame's per-lag sums: the four hop sums, and on an enable's first analysis the tail.
  void frameSums(std::array<float, kRowLen>& d) {
    for (int r = 0; r < kHopSums; ++r) {
      const float* row = hopSums_.data() + r * kRowLen;
      for (int t = 1; t <= kCoarseMax; ++t) d[t] += row[t];
    }
    if (!tail_) return;
    for (int t = 1; t <= kCoarseMax; ++t) d[t] += cmnd_[t];
    tail_ = false;
  }

  // YIN on the 12 kHz frame. Returns whether min d' is below kVoicedThreshold
  // and writes the interpolated candidate lag in decimated samples.
  bool coarse(float& tau) {
    constexpr float threshold = kVoicedThreshold;
    std::array<float, kRowLen> d{};
    frameSums(d);
    float running = 0.0f;
    for (int t = 1; t <= kCoarseMax; ++t) {
      running += d[t];
      cmnd_[t] = running > 0.0f ? d[t] * t / running : 1.0f;
    }
    int best = kCoarseMin;
    int first = -1;
    for (int t = kCoarseMin; t <= kCoarseMax; ++t) {
      if (cmnd_[t] < cmnd_[best]) best = t;
      if (first < 0 && cmnd_[t] < threshold) first = t;
    }
    if (first >= 0) {
      while (first < kCoarseMax && cmnd_[first + 1] < cmnd_[first]) ++first;
    }
    const int pick = first >= 0 ? first : best;
    const float off =
        pick < kCoarseMax ? parabolic(cmnd_[pick - 1], cmnd_[pick], cmnd_[pick + 1]) : 0.0f;
    tau = static_cast<float>(pick) + off;
    lastTau_ = tau;
    lastPeriodic_ = cmnd_[best] < threshold;
    lastClamped_ = pick == kCoarseMax;
    lastOutOfRange_ = lastClamped_ || dipsBelowRange();
    return lastPeriodic_;
  }

  // Adds the per-lag squared differences over one pane whose first sample is x[0] into row. Lags go
  // in pairs so each pass shares its loads; kCoarseMax is odd, so the last lag runs alone.
  static void paneKernel(const float* x, float* row) {
    for (int t = 1; t < kCoarseMax; t += 2) {
      float a = 0.0f, b = 0.0f;
      sumSqDiff2(x, x + t, kPaneLen, a, b);
      row[t] += a;
      row[t + 1] += b;
    }
    row[kCoarseMax] += sumSqDiff(x, x + kCoarseMax, kPaneLen);
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
      if (active_ && staleFrom_ == kPanes) {
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
      replaySpan(h.ring + at, m);
      done += m;
    }
    replaySpan(in, share - fromRing);
    behind_ += n - share;
    untilHop_ -= n;
  }

  // An even share of the work left before the replay catches up, ahead samples from now. A sample
  // past the warm-up counts kPaneWeight times, since those add a pane every block.
  int paceShare(int n, int ahead) const {
    const int left = behind_ + ahead;
    const int work = warm_ + kPaneWeight * (left - warm_);
    const int units = (work * n + ahead - 1) / ahead;
    const int share = units <= warm_ ? units : warm_ + (units - warm_ + kPaneWeight - 1) / kPaneWeight;
    return share < behind_ + n ? share : behind_ + n;
  }

  // m replayed samples. The panes come on where the first analysis's frame starts.
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

  // m samples that end at or before the next block edge of the hop. Both copies of the full-rate
  // ring take them whole. The low-pass runs on a local copy, since stores into the rings could alias
  // its members and force a reload every sample, and every kDecim-th output goes to the decimated
  // ring. The samples' squares add to the block's energy.
  void fill(const float* in, int m) {
    const int first = m < kFullLen - fullPos_ ? m : kFullLen - fullPos_;
    const size_t head = sizeof(float) * static_cast<size_t>(first);
    const size_t tail = sizeof(float) * static_cast<size_t>(m - first);
    std::memcpy(&full_[static_cast<size_t>(fullPos_)], in, head);
    std::memcpy(&full_[static_cast<size_t>(fullPos_ + kFullLen)], in, head);
    std::memcpy(&full_[0], in + first, tail);
    std::memcpy(&full_[kFullLen], in + first, tail);
    fullPos_ = (fullPos_ + m) & (kFullLen - 1);
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
  // pane joins the hop's sum.
  void closeBlock() {
    levels_[static_cast<size_t>(levelRow_)] = energy_;
    levelRow_ = levelRow_ + 1 == kLevelBlocks ? 0 : levelRow_ + 1;
    energy_ = 0.0f;
    if (active_) addPane();
  }

  // The pane that just completed starts kPaneLen + kCoarseMax back. The hop's first pane takes over
  // the sum of the hop four back; before an enable's first analysis that sum's panes are the
  // frame's oldest, so they move to the tail first.
  void addPane() {
    float* row = hopSums_.data() + hopRow_ * kRowLen;
    if (hopCount_ == kBlock) {
      if (tail_) {
        for (int t = 1; t <= kCoarseMax; ++t) cmnd_[t] += row[t];
      }
      std::fill(row, row + kRowLen, 0.0f);
    }
    paneKernel(dec_.data() + decPos_ + kDecLen - (kPaneLen + kCoarseMax), row);
    // The new pane replaces the oldest, so one stale pane fewer is left in the frame.
    if (staleFrom_ < kPanes) ++staleFrom_;
    ++panesThisPush_;
  }

  // A switch clears the result. Switching on zeroes the hop sums, which the next blocks rebuild,
  // and starts the tail empty.
  void setActive(bool active) {
    active_ = active;
    result_ = PitchResult{};
    staleFrom_ = active ? 0 : kPanes;
    if (!active) return;
    hopSums_.fill(0.0f);
    cmnd_.fill(0.0f);
    tail_ = true;
  }

  // Panes of age staleFrom_ and older are missing from the sums. Rebuilds the newest of them, at
  // most kRebuildPanesPerBlock, and none that this call's own pane would push out of the frame.
  void rebuildSome(int n) {
    const int limit = hopCount_ % kBlock + n >= kBlock ? kPanes - 1 : kPanes;
    for (int k = 0; k < kRebuildPanesPerBlock && staleFrom_ < limit; ++k) rebuildPane(staleFrom_++);
  }

  // Age 0 is the newest complete pane. It starts kPaneLen + kCoarseMax back from the last pane
  // boundary, and each older pane kPaneLen further. The open hop holds the newest panes, then each
  // hop back four more; a pane of the hop whose sum the open hop took over goes to the tail.
  void rebuildPane(int age) {
    const int done = hopCount_ / kBlock;
    const int back = age < done ? 0 : 1 + (age - done) / kPanesPerHop;
    const int open = (hopCount_ % kBlock) / kDecim;  // decimated samples into the open pane
    const float* x = dec_.data() + decPos_ + kDecLen - open - (kPaneLen + kCoarseMax) - kPaneLen * age;
    const int row = (hopRow_ + kHopSums - back) % kHopSums;
    paneKernel(x, back < kHopSums ? hopSums_.data() + row * kRowLen : cmnd_.data());
    ++panesThisPush_;
  }

  // The fine pass: the plain difference function at 48 kHz around 4 * tau. Inside the reach it has
  // one dip, so the pass sums the centre lag and its two neighbours, then walks one lag at a time
  // toward the lower neighbour while the sum keeps falling, never past lo or hi. A tie goes to the
  // lower lag, as a scan up from lo would pick. Each lag is summed at most once; the parabola reads
  // the lowest sum and its two neighbours. A minimum on kMinPeriod or kMaxPeriod marks the frame
  // clamped.
  float refine(float tauCoarse) {
    const int centre = static_cast<int>(floorf(tauCoarse * kDecim + 0.5f));
    const int lo = centre - kFineReach < kMinPeriod ? kMinPeriod : centre - kFineReach;
    const int hi = centre + kFineReach > kMaxPeriod ? kMaxPeriod : centre + kFineReach;
    const float* x = fineFrame();
    std::array<float, 2 * kFineReach + 1> d{};
    auto sum = [&](int lag) { return d[lag - lo] = sumSqDiff(x, x + lag, kFineW); };
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

  // Median of the last three voiced periods suppresses single octave jumps.
  void publishVoiced(float period) {
    periods_[nextPeriod_] = period;
    nextPeriod_ = nextPeriod_ == 2 ? 0 : nextPeriod_ + 1;
    if (nPeriods_ < 3) ++nPeriods_;
    float p = period;
    if (nPeriods_ >= 3) {
      const float a = periods_[0], b = periods_[1], c = periods_[2];
      p = fmaxf(fminf(a, b), fminf(fmaxf(a, b), c));
    }
    result_.period = p;
    result_.hz = kSampleRate / p;
    result_.rawHz = kSampleRate / period;
    result_.voiced = true;
  }

  Biquad lp_;  // anti-alias low-pass ahead of the decimator
  std::array<float, 2 * kFullLen> full_{};  // doubled, see fineFrame()
  std::array<float, 2 * kDecLen> dec_{};
  // The last analysis's normalised difference. From an enable to its first analysis it holds the
  // tail instead: the frame's oldest panes, of the hop whose sum the open hop took over.
  std::array<float, kRowLen> cmnd_{};
  bool tail_ = false;
  std::array<float, kHopSums * kRowLen> hopSums_{};  // per-lag sums of each hop's panes
  int hopRow_ = 0;           // the open hop's sum; it moves on at each hop, active or not
  int staleFrom_ = kPanes;  // panes of this age and older still wait for their rebuild
  int panesThisPush_ = 0;
  float lastTau_ = 0.0f;
  bool lastPeriodic_ = false;
  bool lastOutOfRange_ = false;
  bool lastClamped_ = false;
  std::array<float, 3> periods_{};
  int nPeriods_ = 0;
  int nextPeriod_ = 0;
  int fullPos_ = 0;
  int decPos_ = 0;
  int hopCount_ = 0;  // samples into the open hop; the decimator's phase and the pane's follow it
  std::array<float, kLevelBlocks> levels_{};  // energy of each block of the level gate's window
  int levelRow_ = 0;
  float energy_ = 0.0f;  // the open block's energy so far
  bool active_ = false;
  bool fine_ = true;
  PitchResult result_;
  bool rested_ = false;  // the rings missed an idle spell and replay at the next push with a History
  int behind_ = 0;       // samples a replay still runs behind the caller
  int untilHop_ = 0;     // the caller's samples left until a replay's first analysis
  int warm_ = 0;         // replayed samples left before the first analysis's frame starts
};

}  // namespace cv
