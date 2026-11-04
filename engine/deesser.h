#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// De-esser for POLISH EQ's front half. A 4th-order Butterworth band-pass (two biquads) reads the
// sibilant band. Once per 64 samples the detector compares the band level with the full level, both
// in dB, so the singer's volume does not move the trigger. The overshoot past the threshold,
// capped at maxCutDb, sets the gain of a peaking notch in the signal path. A rising cut applies
// to the call that completes the window. A falling cut follows a one-pole release in dB. The
// windows run on across calls, so the cut at each 64th sample is the same however the caller splits
// the audio. The calls before a window completes keep the cut of the window before.
// Spec docs/specs/2026-10-31-polish-eq-adam-voice-design.md.

namespace cv {

// The ranges of a de-esser setting. The box and the emulator stay inside them, and cubevox-render
// refuses a value outside them. A release under the minimum runs at the minimum.
constexpr float kDeEsserThresholdMinDb = -60.0f;
constexpr float kDeEsserThresholdMaxDb = 0.0f;
constexpr float kDeEsserCutMaxDb = 24.0f;
constexpr float kDeEsserReleaseMinMs = 0.1f;
constexpr float kDeEsserReleaseMaxMs = 2000.0f;
constexpr float kDeEsserQMax = 20.0f;

struct DeEsserTuning {
  float bandLoHz = 4500.0f;    // detector band, the -3 dB edges
  float bandHiHz = 7000.0f;
  float thresholdDb = -12.0f;  // band level minus full level where the cut starts
  float maxCutDb = 6.0f;
  float releaseMs = 40.0f;     // time constant of the cut falling
  float notchHz = 5800.0f;
  float notchQ = 1.5f;
};

// True when every field sits inside the ranges above and the filter band.
inline bool deEsserTuningInRange(const DeEsserTuning& t) {
  const auto hz = [](float v) { return v >= kFilterMinHz && v <= kFilterMaxHz; };
  const auto within = [](float v, float lo, float hi) { return v >= lo && v <= hi; };
  return hz(t.bandLoHz) && hz(t.bandHiHz) && hz(t.notchHz) &&
         within(t.thresholdDb, kDeEsserThresholdMinDb, kDeEsserThresholdMaxDb) &&
         within(t.maxCutDb, 0.0f, kDeEsserCutMaxDb) &&
         within(t.releaseMs, kDeEsserReleaseMinMs, kDeEsserReleaseMaxMs) && within(t.notchQ, kPeakMinQ, kDeEsserQMax);
}

class DeEsser {
 public:
  DeEsser() { reset(); }

  void reset() {
    bp_[0].clear();
    bp_[1].clear();
    notch_ = Biquad{};
    cutDb_ = 0.0f;
    sumFull_ = 0.0f;
    sumBand_ = 0.0f;
    count_ = 0;
    notchStale_ = true;
  }

  // Gain reduction of the last window, 0 to the tuning's maxCutDb.
  float cutDb() const { return cutDb_; }

  // De-esses `n` samples in place. Takes n up to kBlock.
  void process(float* x, int n, const DeEsserTuning& t) {
    process(x, n, t, [x](int i, float y) { x[i] = y; });
  }

  // De-esses `n` samples (up to kBlock) and hands each result to emit(i, y). The detector reads the
  // whole call before the notch runs, so `emit` may write over `x`.
  template <typename Emit>
  void process(const float* x, int n, const DeEsserTuning& t, Emit&& emit) {
    if (n <= 0) return;
    detect(x, n, t);
    Biquad notch = notch_;
    for (int i = 0; i < n; ++i) emit(i, notch.run(x[i]));
    notch_ = notch;
  }

 private:
  // Band energy under -60 dBFS RMS never cuts. On an input under -240 dBFS RMS, a filter whose
  // state sits under kFlushLevel clears, so a decay on silence stops before the subnormal range.
  static constexpr float kBandFloorMs = 1.0e-6f;
  static constexpr float kSilenceMs = 1.0e-24f;
  static constexpr float kFlushLevel = 1.0e-15f;
  static constexpr float kTinyMs = 1.0e-30f;
  static constexpr float kCutSnapDb = 0.01f;
  static constexpr float kInvBlock = 1.0f / static_cast<float>(kBlock);

  static bool sameTuning(const DeEsserTuning& a, const DeEsserTuning& b) {
    return a.bandLoHz == b.bandLoHz && a.bandHiHz == b.bandHiHz && a.thresholdDb == b.thresholdDb &&
           a.maxCutDb == b.maxCutDb && a.releaseMs == b.releaseMs && a.notchHz == b.notchHz &&
           a.notchQ == b.notchQ;
  }

  // Runs the detector over the call and closes the window when it reaches kBlock samples, then
  // refreshes the notch if the cut moved.
  void detect(const float* x, int n, const DeEsserTuning& t) {
    if (!designed_ || !sameTuning(t, applied_)) design(t);
    const int head = std::min(n, kBlock - count_);
    accumulate(x, head);
    if (count_ == kBlock) closeWindow(t);
    accumulate(x + head, n - head);
    if (notchStale_ || cutDb_ != appliedCutDb_) setNotch(cutDb_);
  }

  // Adds the band and full energy of `n` samples to the window. The filter states stay in locals.
  // The sums are fused multiply-adds by name, so the compiler cannot split them into a separate
  // multiply and add when it pairs the two sums in one vector.
  void accumulate(const float* x, int n) {
    Biquad b0 = bp_[0];
    Biquad b1 = bp_[1];
    float sumFull = sumFull_;
    float sumBand = sumBand_;
    for (int i = 0; i < n; ++i) {
      const float b = b1.run(b0.run(x[i]));
      sumFull = std::fma(x[i], x[i], sumFull);
      sumBand = std::fma(b, b, sumBand);
    }
    bp_[0] = b0;
    bp_[1] = b1;
    sumFull_ = sumFull;
    sumBand_ = sumBand;
    count_ += n;
  }

  void closeWindow(const DeEsserTuning& t) {
    const float msFull = sumFull_ * kInvBlock;
    if (msFull < kSilenceMs) flushQuiet();
    updateCut(sumBand_ * kInvBlock, msFull, t);
    sumFull_ = 0.0f;
    sumBand_ = 0.0f;
    count_ = 0;
  }

  // Clears each filter whose state has decayed under kFlushLevel. Each filter decays at its own
  // rate, so each one is judged alone.
  void flushQuiet() {
    for (Biquad* b : {&bp_[0], &bp_[1], &notch_}) {
      if (fabsf(b->z1) < kFlushLevel && fabsf(b->z2) < kFlushLevel) b->clear();
    }
  }

  void updateCut(float msBand, float msFull, const DeEsserTuning& t) {
    float target = 0.0f;
    if (msBand >= kBandFloorMs) {
      const float levelDb = (kDbPerLog2 / 2) * (log2Fast(msBand) - log2Fast(msFull + kTinyMs));  // power to dB
      target = clampf(levelDb - t.thresholdDb, 0.0f, t.maxCutDb);
    }
    if (target >= cutDb_) {
      cutDb_ = target;
      return;
    }
    cutDb_ = target + (cutDb_ - target) * releaseKeep_;
    if (cutDb_ - target < kCutSnapDb) cutDb_ = target;
  }

  // The peaking notch at the tuning's frequency with gain -cutDb. The corner comes from the tuning,
  // so a block costs one exp2 and the biquad's divides.
  void setNotch(float cutDb) {
    notch_.setPeakAmp(notchCorner_, exp2Fast(-cutDb * (0.5f * kLog2PerDb)), applied_.notchQ);
    appliedCutDb_ = cutDb;
    notchStale_ = false;
  }

  void design(const DeEsserTuning& t) {
    designBand(t.bandLoHz, t.bandHiHz);
    notchCorner_ = rbjCorner(t.notchHz);
    const float releaseSec = fmaxf(t.releaseMs, kDeEsserReleaseMinMs) * 0.001f;
    releaseKeep_ = smooth::keep(releaseSec, kBlock);
    notchStale_ = true;
    applied_ = t;
    designed_ = true;
  }

  // Butterworth band-pass of order 2 on each edge, from the analog low-pass poles at 135 degrees
  // mapped to the band and then through the bilinear transform. The two poles of the low-pass
  // pair become two pole pairs, one biquad each, and each biquad has unity gain at the centre.
  void designBand(float loHz, float hiHz) {
    const float lo = clampf(loHz, kFilterMinHz, 0.4f * kSampleRate);
    const float hi = clampf(hiHz, 1.05f * lo, kFilterMaxHz);
    const float o1 = tanf(kPi * lo / kSampleRate);
    const float o2 = tanf(kPi * hi / kSampleRate);
    const float centreSq = o1 * o2;
    const float u = (o2 - o1) * 0.35355339f;  // half the analog bandwidth over sqrt 2
    const float m = sqrtf(centreSq * centreSq + 4.0f * u * u * u * u);
    const float rRe = u * u * sqrtf(2.0f / (m + centreSq));
    const float rIm = sqrtf(0.5f * (m + centreSq));
    bandSection(bp_[0], -u + rRe, u - rIm, centreSq);
    bandSection(bp_[1], -u - rRe, u + rIm, centreSq);
  }

  // Digital biquad for the analog pole pair re +- j im, with the zero at DC and unity gain at the
  // analog centre. The section keeps its running state.
  static void bandSection(Biquad& s, float re, float im, float centreSq) {
    const float c1 = -2.0f * re;
    const float c0 = re * re + im * im;
    const float off = c0 - centreSq;
    const float gain = sqrtf((off * off + c1 * c1 * centreSq) / centreSq);
    const float inv = 1.0f / (1.0f + c1 + c0);
    s.b0 = gain * inv;
    s.b1 = 0.0f;
    s.b2 = -gain * inv;
    s.a1 = 2.0f * (c0 - 1.0f) * inv;
    s.a2 = (1.0f - c1 + c0) * inv;
  }

  std::array<Biquad, 2> bp_;
  Biquad notch_;
  RbjCorner notchCorner_{1.0f, 0.0f};
  float cutDb_ = 0.0f;
  float appliedCutDb_ = 0.0f;
  float releaseKeep_ = 1.0f;  // the release decay over one kBlock
  float sumFull_ = 0.0f;      // energy sums of the open window
  float sumBand_ = 0.0f;
  int count_ = 0;             // samples in the open window
  bool notchStale_ = true;
  bool designed_ = false;
  DeEsserTuning applied_;
};

}  // namespace cv
