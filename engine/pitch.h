#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"

// Monophonic pitch tracker. Two-stage YIN: a coarse search on a 12 kHz copy
// finds the period cheaply, then a narrow search at 48 kHz refines it.

namespace cv {

struct PitchResult {
  float period = 0.0f;   // samples at kSampleRate; 0 until first voiced frame
  float hz = 0.0f;
  float rawHz = 0.0f;    // newest frame before the median; one hop fresher than hz
  bool voiced = false;
};

class PitchTracker {
 public:
  static constexpr int kHop = 256;
  static constexpr float kMinHz = 70.0f;
  static constexpr float kMaxHz = 1000.0f;
  static constexpr float kVoicedThreshold = 0.15f;  // YIN threshold on the coarse search

  // A frame rebuilt on enable takes this many panes per block on top of the block's own pane, so
  // no single block carries the whole rebuild.
  static constexpr int kRebuildPanesPerBlock = 2;

  PitchTracker() {
    lp_.setLowPass(kLowpassHz, kButterworthQ);
    reset();
  }

  void reset() {
    full_.fill(0.0f);
    dec_.fill(0.0f);
    fullPos_ = 0;
    decPos_ = 0;
    decPhase_ = 0;
    panes_.fill(0.0f);
    paneRow_ = 0;
    paneFill_ = 0;
    staleFrom_ = kPanes;
    panesThisPush_ = 0;
    hopCount_ = 0;
    lp_.clear();
    periods_.fill(0.0f);
    nPeriods_ = 0;
    nextPeriod_ = 0;
    active_ = false;
    result_ = PitchResult{};
  }

  // Feed n <= kBlock samples. The rings always fill, so the frame is warm. Analysis runs every
  // kHop samples while active and the panes are current. An enable rebuilds the panes over the
  // next few blocks and analyses at the end of the block that makes them whole, unless a hop
  // already did in that block. Inactive, the result reads unvoiced.
  void push(const float* in, int n, bool active = true) {
    panesThisPush_ = 0;
    if (active != active_) {
      active_ = active;
      result_ = PitchResult{};
      staleFrom_ = active ? 0 : kPanes;
    }
    const bool stale = active_ && staleFrom_ < kPanes;
    if (stale) rebuildSome();
    bool analysed = false;
    for (int i = 0; i < n; ++i) {
      full_[fullPos_] = in[i];
      full_[fullPos_ + kFullLen] = in[i];
      fullPos_ = (fullPos_ + 1) & (kFullLen - 1);
      const float y = lp_.run(in[i]);
      if (++decPhase_ == kDecim) {
        decPhase_ = 0;
        pushDecimated(y);
      }
      if (++hopCount_ == kHop) {
        hopCount_ = 0;
        if (active_ && staleFrom_ == kPanes) {
          analyse();
          analysed = true;
        }
      }
    }
    if (stale && staleFrom_ == kPanes && !analysed) analyse();
  }

  const PitchResult& result() const { return result_; }

  // Test hooks.
  static constexpr int kCoarseWForTest = 256;
  static constexpr int kCoarseMinForTest = 12;
  static constexpr int kCoarseMaxForTest = 171;
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
  static constexpr int kPaneLen = kBlock / kDecim;
  static constexpr int kPanes = kCoarseW / kPaneLen;
  static constexpr int kFineW = 1024;
  static constexpr int kFineMin = 48;
  static constexpr int kFineMax = 686;
  static constexpr int kFineReach = 4;
  static constexpr int kFineSpan = kFineW + kFineMax;
  static constexpr float kMinRms = 0.0031623f;  // -50 dBFS
  static constexpr float kLowpassHz = 2500.0f;

  static_assert(kCoarseW % 4 == 0 && kFineW % 4 == 0, "windows must unroll by four");
  static_assert(kCoarseMax % 2 == 1, "the paired lag loop leaves one lag for the tail");
  static_assert(kCoarseSpan <= kDecLen, "coarse frame exceeds decimated ring");
  static_assert(kBlock % kDecim == 0, "a block is a whole number of decimated samples");
  static_assert(kCoarseW % kPaneLen == 0, "the coarse window is a whole number of panes");
  static_assert(kHop % (kPaneLen * kDecim) == 0, "hops land on pane boundaries");
  static_assert(kCoarseSpan + kPaneLen <= kDecLen, "pane reach exceeds decimated ring");
  static_assert(kPaneLen % 4 == 0, "panes must unroll by four");
  static_assert(kCoarseWForTest == kCoarseW && kCoarseMinForTest == kCoarseMin &&
                    kCoarseMaxForTest == kCoarseMax,
                "test hooks mirror the coarse constants");
  static_assert(kFineSpan <= kFullLen, "fine frame exceeds full-rate ring");
  static_assert(kRebuildPanesPerBlock >= 1, "an enable must make progress on its rebuild");

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

  static float sumSq(const float* a, int n) {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    for (int j = 0; j < n; j += 4) {
      s0 += a[j] * a[j];
      s1 += a[j + 1] * a[j + 1];
      s2 += a[j + 2] * a[j + 2];
      s3 += a[j + 3] * a[j + 3];
    }
    return (s0 + s1) + (s2 + s3);
  }

  float frameRms() const {
    return sqrtf(sumSq(fineFrame() + (kFineSpan - kFineW), kFineW) / kFineW);
  }

  void analyse() {
    float tau = 0.0f;
    const bool periodic = coarse(tau);
    if (periodic && frameRms() > kMinRms) {
      publishVoiced(refine(tau));
    } else {
      result_.voiced = false;
    }
  }

  // YIN on the 12 kHz frame. Returns whether min d' is below kVoicedThreshold
  // and writes the interpolated candidate lag in decimated samples.
  bool coarse(float& tau) {
    constexpr float threshold = kVoicedThreshold;
    std::array<float, kCoarseMax + 1> d{};
    for (int r = 0; r < kPanes; ++r) {
      const float* row = panes_.data() + r * (kCoarseMax + 1);
      for (int t = 1; t <= kCoarseMax; ++t) d[t] += row[t];
    }
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
    return lastPeriodic_;
  }

  // Per-lag squared differences over one pane whose first sample is x[0].
  // Lags go in pairs so each pass shares its loads; kCoarseMax is odd, so the
  // last lag runs alone.
  static void paneKernel(const float* x, float* row) {
    for (int t = 1; t < kCoarseMax; t += 2) sumSqDiff2(x, x + t, kPaneLen, row[t], row[t + 1]);
    row[kCoarseMax] = sumSqDiff(x, x + kCoarseMax, kPaneLen);
  }

  // Writes one decimated sample. While active, each completed pane gets its row at once.
  void pushDecimated(float y) {
    dec_[decPos_] = y;
    dec_[decPos_ + kDecLen] = y;
    decPos_ = (decPos_ + 1) & (kDecLen - 1);
    if (++paneFill_ < kPaneLen) return;
    paneFill_ = 0;
    if (!active_) return;
    computePane();
    // The new pane replaces the oldest, so one stale pane fewer is left in the frame.
    if (staleFrom_ < kPanes) ++staleFrom_;
  }

  // The pane that just completed: its first sample is kPaneLen + kCoarseMax back.
  void computePane() {
    const float* x = dec_.data() + decPos_ + kDecLen - (kPaneLen + kCoarseMax);
    paneKernel(x, panes_.data() + paneRow_ * (kCoarseMax + 1));
    paneRow_ = paneRow_ + 1 == kPanes ? 0 : paneRow_ + 1;
    ++panesThisPush_;
  }

  // Panes of age staleFrom_ and older hold rows from before the enable. Rebuilds the newest of
  // them, at most kRebuildPanesPerBlock.
  void rebuildSome() {
    for (int k = 0; k < kRebuildPanesPerBlock && staleFrom_ < kPanes; ++k) rebuildPane(staleFrom_++);
  }

  // Age 0 is the newest complete pane. It starts kPaneLen + kCoarseMax back from the last pane
  // boundary, and each older pane kPaneLen further; the oldest row is the one paneRow_ points at.
  void rebuildPane(int age) {
    const float* x =
        dec_.data() + decPos_ + kDecLen - paneFill_ - (kPaneLen + kCoarseMax) - kPaneLen * age;
    const int row = (paneRow_ + kPanes - 1 - age) % kPanes;
    paneKernel(x, panes_.data() + row * (kCoarseMax + 1));
    ++panesThisPush_;
  }

  // Plain difference function at 48 kHz over a few lags around 4 * tau.
  float refine(float tauCoarse) {
    const int centre = static_cast<int>(floorf(tauCoarse * kDecim + 0.5f));
    const int lo = centre - kFineReach < kFineMin ? kFineMin : centre - kFineReach;
    const int hi = centre + kFineReach > kFineMax ? kFineMax : centre + kFineReach;
    std::array<float, 2 * kFineReach + 1> d{};
    int m = 0;
    const float* x = fineFrame();
    for (int lag = lo; lag <= hi; ++lag) {
      const float s = sumSqDiff(x, x + lag, kFineW);
      d[lag - lo] = s;
      if (s < d[m]) m = lag - lo;
    }
    const float off = (m > 0 && lo + m < hi) ? parabolic(d[m - 1], d[m], d[m + 1]) : 0.0f;
    return static_cast<float>(lo + m) + off;
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
  std::array<float, kCoarseMax + 1> cmnd_{};
  std::array<float, kPanes * (kCoarseMax + 1)> panes_{};  // rows of per-lag sums, index 0 unused
  int paneRow_ = 0;
  int paneFill_ = 0;
  int staleFrom_ = kPanes;  // panes of this age and older still wait for their rebuild
  int panesThisPush_ = 0;
  float lastTau_ = 0.0f;
  bool lastPeriodic_ = false;
  std::array<float, 3> periods_{};
  int nPeriods_ = 0;
  int nextPeriod_ = 0;
  int fullPos_ = 0;
  int decPos_ = 0;
  int decPhase_ = 0;
  int hopCount_ = 0;
  bool active_ = false;
  PitchResult result_;
};

}  // namespace cv
