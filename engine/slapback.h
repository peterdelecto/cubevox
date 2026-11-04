#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Echo: a feedback delay on the voice. Time (knob A) sets the delay, and the repeats pass through a
// two-pole low-pass inside the loop. Amount (knob B) sets the echo level and the repeat gain
// together. The dry voice passes at unity at every Amount; the trim scales the echo alone. Repeats
// land on the tuned feedback at Amount 25 % and grow as sqrt(Amount), capped at kFeedbackMax.
//
// A knob touch on Time glides the delay with a 50 ms time constant, so the repeat bends in pitch
// while it moves.

namespace cv {

struct SlapbackTuning {
  float timeMs = 100.0f;
  float lowpassHz = 4000.0f;
  float feedback = 0.35f;  // repeat gain at Amount 25 %, about 2.5 audible repeats
  float wetMaxDb = 4.9f;   // echo level at Amount 1, before the trim
  float trimDb = -5.3f;    // gain on the echo alone; the level row at Amount 25 % sets it
};

struct SlapbackParams {
  bool on = true;
  float amount = 0.0f;  // panel AMOUNT 0..1
  SlapbackTuning tuning;
};

class Slapback {
 public:
  // The Time knob's range. The registry and the tests read it from here.
  static constexpr float kTimeMinMs = 60.0f;
  static constexpr float kTimeMaxMs = 250.0f;

  // The ranges the engine clamps the other tuning values to.
  static constexpr float kCutoffMinHz = 500.0f;
  static constexpr float kCutoffMaxHz = 12000.0f;
  static constexpr float kFeedbackMax = 0.5f;
  static constexpr float kLevelLimitDb = 24.0f;  // wetMaxDb and trimDb, either side of 0

  void reset() {
    lp_.clear();
    amount_ = 0.0f;
    timeBase_ = 0.0f;
    timeErr_ = 0.0f;
    timeValid_ = false;
    writePos_ = 0;
    filled_ = false;
  }

  // Runs in chunks of at most kBlock samples, so any n works.
  void process(const float* in, float* out, int n, const SlapbackParams& p) {
    for (int off = 0; off < n; off += kBlock) chunk(in + off, out + off, std::min(kBlock, n - off), p);
  }

 private:
  static constexpr float kFeedbackRefAmount = 0.25f;  // Amount where the tuned feedback applies
  static constexpr float kTimeSec = 0.050f;

  // The line holds the longest delay, one chunk for the segment loops to stay clear of the write
  // head, and the four taps of the cubic read. The guard repeats the first samples past the end so
  // four taps read contiguously.
  static constexpr int kReadTaps = 4;
  static constexpr int kGuard = 3;
  static constexpr int kMaxDelaySmp = static_cast<int>(kTimeMaxMs * kSmpPerMs);
  static constexpr int kLen = kMaxDelaySmp + kBlock + kReadTaps;

  // The Time target is clamped to the knob's range, and the glide never leaves the span between two
  // targets, so the read needs no clamp of its own. The shortest delay clears a chunk and the guard.
  static_assert(kTimeMinMs * kSmpPerMs > kBlock + kGuard, "the shortest delay keeps a chunk and its taps behind the write head");
  static_assert(kLen - kMaxDelaySmp >= kBlock, "a segment read never reaches the segment it writes");

  // Per-sample decay of the Time glide's error: what the 50 ms smoother keeps in one sample.
  static inline const float kTimeDecay = smooth::keep(kTimeSec, 1);

  // log2 of the per-sample decay of the Amount smoother, so a chunk of n samples decays by 2^(n x this).
  static constexpr float kAmountDecayLog2 = -1.0f / (smooth::kSmoothSec * kSampleRate) * kLog2E;

  // The repeat low-pass in direct form I. A low-pass has b1 = 2 b0 and b2 = b0, so the feed-forward
  // sum and the a2 term are ready before y1 arrives, and the recursion through y is one
  // multiply-add long. The coefficients come from the RBJ low-pass, the same transfer function.
  struct Filter {
    float b0 = 1.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float x1 = 0.0f;
    float x2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;

    void set(float hz) {
      Biquad b;
      b.setLowPass(hz, kButterworthQ);
      b0 = b.b0;
      a1 = b.a1;
      a2 = b.a2;
    }

    CV_INLINE float run(float x) {
      const float u = x + 2.0f * x1 + x2;
      const float y = (b0 * u - a2 * y2) - a1 * y1;
      x2 = x1;
      x1 = x;
      y2 = y1;
      y1 = y;
      return y;
    }

    void clear() { x1 = x2 = y1 = y2 = 0.0f; }
  };

  // Everything that follows Amount, at one Amount value.
  struct Edge {
    float wet;       // echo gain, trim included
    float feedback;  // repeats
  };

  // The echo's gain at Amount 1: its level and its trim.
  static float echoMaxGain(const SlapbackTuning& t) {
    return dbToGainFast(clampf(t.wetMaxDb, -kLevelLimitDb, kLevelLimitDb) +
                        clampf(t.trimDb, -kLevelLimitDb, kLevelLimitDb));
  }

  static Edge edgeAt(const SlapbackTuning& t, float echoMax, float amount) {
    const float feedbackRef = clampf(t.feedback, 0.0f, kFeedbackMax);
    return {amount * echoMax, fminf(kFeedbackMax, feedbackRef * sqrtf(amount / kFeedbackRefAmount))};
  }

  // The edge at a settled Amount, recomputed only when Amount or a tuning value it reads changes.
  Edge settledEdge(const SlapbackTuning& t, float amount) {
    if (!edgeValid_ || edgeKey_[0] != amount || edgeKey_[1] != t.feedback || edgeKey_[2] != t.wetMaxDb ||
        edgeKey_[3] != t.trimDb) {
      edge_ = edgeAt(t, echoMaxGain(t), amount);
      edgeKey_ = {amount, t.feedback, t.wetMaxDb, t.trimDb};
      edgeValid_ = true;
    }
    return edge_;
  }

  void setLowPass(float hz) {
    if (hz == lpHz_) return;
    lp_.set(hz);
    lpHz_ = hz;
  }

  void chunk(const float* CV_RESTRICT in, float* CV_RESTRICT out, int n, const SlapbackParams& p) {
    const SlapbackTuning& t = p.tuning;
    const float target = p.on ? smooth::clamp01(p.amount) : 0.0f;
    // The target is a whole number of samples, so a settled read is one tap with no interpolation.
    // The knob steps 2 samples at a time, and the delay is off the knob's value by at most half a sample.
    const float timeTarget = std::nearbyint(fminf(fmaxf(t.timeMs, kTimeMinMs), kTimeMaxMs) * kSmpPerMs);
    setLowPass(clampf(t.lowpassHz, kCutoffMinHz, kCutoffMaxHz));

    // The delay is timeBase_ + timeErr_. The first chunk after reset lands on the tuned time, and a
    // new target moves the base and carries the difference in the error, so the delay stays put and
    // then glides. The error decays to exactly 0, so the delay lands on the target.
    if (!timeValid_) {
      timeBase_ = timeTarget;
      timeErr_ = 0.0f;
      timeValid_ = true;
    }
    if (timeTarget != timeBase_) {
      timeErr_ += timeBase_ - timeTarget;
      timeBase_ = timeTarget;
    }

    if (target == 0.0f && amount_ == 0.0f) {
      idle(in, out, n);
      return;
    }

    // Until the write head first wraps, taps behind the start of the line are older than the
    // recording and read zero, so a reset needs no clear. That window runs on the moving path.
    if (!filled_) {
      moving<true>(in, out, n, t, target);
      return;
    }
    for (int j = 0; j < kGuard; ++j) line_[static_cast<size_t>(kLen + j)] = line_[static_cast<size_t>(j)];
    if (amount_ == target && timeErr_ == 0.0f) {
      fixed(in, out, n, settledEdge(t, target));
    } else {
      moving<false>(in, out, n, t, target);
    }
  }

  // Amount parked at 0: the output is the input, and the line keeps recording so the first repeats
  // after the knob lifts are right. The low-pass restarts from zero and the Time glide lands,
  // because nothing is heard.
  void idle(const float* CV_RESTRICT in, float* CV_RESTRICT out, int n) {
    lp_.clear();
    timeErr_ = 0.0f;
    int w = writePos_;
    int done = 0;
    while (done < n) {
      const int m = std::min(n - done, kLen - w);
      std::memcpy(line_.data() + w, in + done, static_cast<size_t>(m) * sizeof(float));
      std::memcpy(out + done, in + done, static_cast<size_t>(m) * sizeof(float));
      done += m;
      w += m;
      if (w == kLen) {
        w = 0;
        filled_ = true;
      }
    }
    writePos_ = w;
  }

  // Settled Amount and settled Time: one tap and constant gains. The read runs behind the write in
  // segments that end where either pointer wraps.
  void fixed(const float* CV_RESTRICT in, float* CV_RESTRICT out, int n, const Edge& e) {
    const int lag = static_cast<int>(timeBase_);
    float* const line = line_.data();
    int ri = writePos_ - lag;
    if (ri < 0) ri += kLen;
    int wi = writePos_;
    int done = 0;
    while (done < n) {
      const int m = std::min(n - done, std::min(kLen - wi, kLen - ri));
      segment(in + done, out + done, m, line + ri, line + wi, e);
      done += m;
      wi += m;
      ri += m;
      if (wi == kLen) wi = 0;
      if (ri == kLen) ri = 0;
    }
    writePos_ = wi;
  }

  void segment(const float* CV_RESTRICT in, float* CV_RESTRICT out, int m, const float* CV_RESTRICT rd,
               float* CV_RESTRICT wr, const Edge& e) {
    Filter lp = lp_;
    int k = 0;

    // Two samples a pass, so the filter's four state values rotate through registers without moves.
    {
      const float b0 = lp.b0;
      const float a1 = lp.a1;
      const float a2 = lp.a2;
      float fx1 = lp.x1;
      float fx2 = lp.x2;
      float fy1 = lp.y1;
      float fy2 = lp.y2;
      for (; k + 1 < m; k += 2) {
        const float ra = rd[k];
        const float rb = rd[k + 1];
        const float ya = (b0 * (ra + 2.0f * fx1 + fx2) - a2 * fy2) - a1 * fy1;
        const float yb = (b0 * (rb + 2.0f * ra + fx1) - a2 * fy1) - a1 * ya;
        const float ia = in[k];
        const float ib = in[k + 1];
        wr[k] = ia + e.feedback * ya;
        wr[k + 1] = ib + e.feedback * yb;
        out[k] = ia + e.wet * ya;
        out[k + 1] = ib + e.wet * yb;
        fx2 = ra;
        fx1 = rb;
        fy2 = ya;
        fy1 = yb;
      }
      lp.x1 = fx1;
      lp.x2 = fx2;
      lp.y1 = fy1;
      lp.y2 = fy2;
    }
    for (; k < m; ++k) {
      const float echo = lp.run(rd[k]);
      const float x = in[k];
      wr[k] = x + e.feedback * echo;
      out[k] = x + e.wet * echo;
    }
    lp_ = lp;
  }

  // Amount moving or Time still gliding: the delay and the gains change every sample.
  template <bool Warm>
  void moving(const float* CV_RESTRICT in, float* CV_RESTRICT out, int n, const SlapbackTuning& t, float target) {
    Filter lp = lp_;
    float timeErr = timeErr_;
    const float timeBase = timeBase_;
    float* const line = line_.data();
    int w = writePos_;
    int count = w;  // Warm: samples recorded since reset
    float amountEnd = amount_;
    Edge e0;
    Edge e1;
    if (amount_ == target) {
      e0 = e1 = settledEdge(t, target);
    } else {
      // The smoother's n steps in one: the distance to the target shrinks by 2^(n x kAmountDecayLog2).
      amountEnd = target + (amount_ - target) * exp2Fast(kAmountDecayLog2 * static_cast<float>(n));
      if (fabsf(target - amountEnd) < smooth::kSnap) amountEnd = target;
      const float echoMax = echoMaxGain(t);
      e0 = edgeAt(t, echoMax, amount_);
      e1 = edgeAt(t, echoMax, amountEnd);
    }

    // The gains step from one edge to the next in n equal increments.
    const float inv = 1.0f / static_cast<float>(n);
    const float dFeedback = (e1.feedback - e0.feedback) * inv;
    const float dWet = (e1.wet - e0.wet) * inv;
    float feedback = e0.feedback;
    float wetGain = e0.wet;
    for (int i = 0; i < n; ++i) {
      const float x = in[i];
      feedback += dFeedback;
      wetGain += dWet;
      timeErr *= kTimeDecay;
      const float delay = timeBase + timeErr;
      float raw;
      if constexpr (Warm) {
        raw = readWarm(line, count, delay);
      } else {
        raw = readCubic(line, w, delay);
      }
      const float echo = lp.run(raw);
      line[w] = x + feedback * echo;
      out[i] = x + wetGain * echo;
      w = w + 1 == kLen ? 0 : w + 1;
      if constexpr (Warm) ++count;
    }
    if (timeBase + timeErr == timeBase) timeErr = 0.0f;
    timeErr_ = timeErr;
    lp_ = lp;
    writePos_ = w;
    amount_ = amountEnd;
    if constexpr (Warm) {
      if (count >= kLen) filled_ = true;
    }
  }

  // Catmull-Rom read `delay` samples behind the write head, through the guard past the line's end.
  static CV_INLINE float readCubic(const float* line, int w, float delay) {
    const float pos = static_cast<float>(w) - delay;
    const float fl = floorf(pos);
    int j = static_cast<int>(fl) - 1;
    if (j < 0) j += kLen;
    j = static_cast<unsigned>(j) < static_cast<unsigned>(kLen) ? j : 0;
    return catmullRom(line[j], line[j + 1], line[j + 2], line[j + 3], pos - fl);
  }

  // The same read while the line is filling, with the position counted from the reset. A tap before
  // the first sample reads zero.
  static CV_INLINE float readWarm(const float* line, int count, float delay) {
    const float pos = static_cast<float>(count) - delay;
    const float fl = floorf(pos);
    const int q = static_cast<int>(fl) - 1;
    return catmullRom(warmTap(line, q), warmTap(line, q + 1), warmTap(line, q + 2), warmTap(line, q + 3), pos - fl);
  }

  // Sample q since reset sits at q mod kLen; a negative q is before the recording.
  static CV_INLINE float warmTap(const float* line, int q) {
    return static_cast<unsigned>(q) < static_cast<unsigned>(2 * kLen) ? line[q >= kLen ? q - kLen : q] : 0.0f;
  }

  std::array<float, kLen + kGuard> line_{};  // reset leaves it alone; filled_ and writePos_ say how much is recorded
  Filter lp_;                                // repeat low-pass
  float lpHz_ = 0.0f;                        // the cutoff lp_ holds, 0 before the first chunk
  Edge edge_{};
  std::array<float, 4> edgeKey_{};  // Amount, feedback, wetMaxDb and trimDb that edge_ was built from
  bool edgeValid_ = false;
  float amount_ = 0.0f;
  float timeBase_ = 0.0f;  // the Time target in samples
  float timeErr_ = 0.0f;   // the glide's distance from the target, in samples
  bool timeValid_ = false;
  int writePos_ = 0;
  bool filled_ = false;  // the write head has wrapped since reset
};

}  // namespace cv
