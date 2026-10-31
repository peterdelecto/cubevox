#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"

// De-esser for POLISH EQ's front half. A 4th-order Butterworth band-pass (two biquads) reads the
// sibilant band. Once per block the detector compares the band level with the full level, both
// in dB, so the singer's volume does not move the trigger. The overshoot past the threshold,
// capped at maxCutDb, sets the gain of a peaking notch in the signal path. A rising cut applies
// within the block. A falling cut follows a one-pole release in dB.
// Spec docs/specs/2026-10-31-polish-eq-adam-voice-design.md.

namespace cv {

struct DeEsserTuning {
  float bandLoHz = 4500.0f;    // detector band, the -3 dB edges
  float bandHiHz = 7000.0f;
  float thresholdDb = -12.0f;  // band level minus full level where the cut starts
  float maxCutDb = 6.0f;
  float releaseMs = 40.0f;     // time constant of the cut falling
  float notchHz = 5800.0f;
  float notchQ = 1.5f;
};

class DeEsser {
 public:
  DeEsser() { reset(); }

  void reset() {
    bp_[0].clear();
    bp_[1].clear();
    notch_ = Biquad{};
    cutDb_ = 0.0f;
    notchStale_ = true;
  }

  // Gain reduction of the last block, 0 to the tuning's maxCutDb.
  float cutDb() const { return cutDb_; }

  // De-esses `n` samples in place. Takes n up to kBlock, one detector reading per call.
  void process(float* x, int n, const DeEsserTuning& t) {
    if (n <= 0) return;
    if (!designed_ || !sameTuning(t, applied_)) design(t);

    float sumFull = 0.0f;
    float sumBand = 0.0f;
    for (int i = 0; i < n; ++i) {
      const float b = bp_[1].run(bp_[0].run(x[i]));
      sumFull += x[i] * x[i];
      sumBand += b * b;
    }
    const float scale = 1.0f / static_cast<float>(n);
    if (sumFull * scale < kSilenceMs) flushQuiet();
    updateCut(sumBand * scale, sumFull * scale, n, t);
    if (notchStale_ || cutDb_ != appliedCutDb_) setNotch(cutDb_);
    for (int i = 0; i < n; ++i) x[i] = notch_.run(x[i]);
  }

 private:
  // Band energy under -60 dBFS RMS never cuts. On an input under -240 dBFS RMS, a filter whose
  // state sits under kFlushLevel clears, so a decay on silence stops before the subnormal range.
  static constexpr float kBandFloorMs = 1.0e-6f;
  static constexpr float kSilenceMs = 1.0e-24f;
  static constexpr float kFlushLevel = 1.0e-15f;
  static constexpr float kTinyMs = 1.0e-30f;
  static constexpr float kCutSnapDb = 0.01f;
  static constexpr float kPowerDbPerLog2 = 0.5f * kDbPerLog2;  // 10 log10(2)

  static bool sameTuning(const DeEsserTuning& a, const DeEsserTuning& b) {
    return a.bandLoHz == b.bandLoHz && a.bandHiHz == b.bandHiHz && a.thresholdDb == b.thresholdDb &&
           a.maxCutDb == b.maxCutDb && a.releaseMs == b.releaseMs && a.notchHz == b.notchHz &&
           a.notchQ == b.notchQ;
  }

  // Clears each filter whose state has decayed under kFlushLevel. Each filter decays at its own
  // rate, so each one is judged alone.
  void flushQuiet() {
    for (Biquad* b : {&bp_[0], &bp_[1], &notch_}) {
      if (fabsf(b->z1) < kFlushLevel && fabsf(b->z2) < kFlushLevel) b->clear();
    }
  }

  void updateCut(float msBand, float msFull, int n, const DeEsserTuning& t) {
    float target = 0.0f;
    if (msBand >= kBandFloorMs) {
      const float levelDb = kPowerDbPerLog2 * (log2Fast(msBand) - log2Fast(msFull + kTinyMs));
      target = clampf(levelDb - t.thresholdDb, 0.0f, t.maxCutDb);
    }
    if (target >= cutDb_) {
      cutDb_ = target;
      return;
    }
    const float keep = n == kBlock ? releaseKeep_ : exp2Fast(static_cast<float>(n) * releaseLog2_);
    cutDb_ = target + (cutDb_ - target) * keep;
    if (cutDb_ - target < kCutSnapDb) cutDb_ = target;
  }

  // RBJ peaking filter at the notch frequency with gain -cutDb. The sine and cosine come from the
  // tuning, so a block costs one exp2 and one divide.
  void setNotch(float cutDb) {
    const float amp = exp2Fast(-cutDb * (0.5f * kLog2PerDb));
    const float inv = 1.0f / (1.0f + notchAlpha_ / amp);
    notch_.b0 = (1.0f + notchAlpha_ * amp) * inv;
    notch_.b1 = -2.0f * notchCos_ * inv;
    notch_.b2 = (1.0f - notchAlpha_ * amp) * inv;
    notch_.a1 = notch_.b1;
    notch_.a2 = (1.0f - notchAlpha_ / amp) * inv;
    appliedCutDb_ = cutDb;
    notchStale_ = false;
  }

  void design(const DeEsserTuning& t) {
    designBand(t.bandLoHz, t.bandHiHz);
    const float w0 = kTwoPi * clampf(t.notchHz, kFilterMinHz, kFilterMaxHz) / kSampleRate;
    notchCos_ = cosf(w0);
    notchAlpha_ = sinf(w0) / (2.0f * (t.notchQ < 0.1f ? 0.1f : t.notchQ));
    const float releaseSec = (t.releaseMs < 0.1f ? 0.1f : t.releaseMs) * 0.001f;
    releaseLog2_ = -1.4426950409f / (releaseSec * kSampleRate);  // log2(e) per sample
    releaseKeep_ = exp2f(static_cast<float>(kBlock) * releaseLog2_);
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
  float cutDb_ = 0.0f;
  float appliedCutDb_ = 0.0f;
  float notchCos_ = 1.0f;
  float notchAlpha_ = 0.0f;
  float releaseLog2_ = 0.0f;  // log2 of the per-sample release decay
  float releaseKeep_ = 1.0f;  // the release decay over one kBlock
  bool notchStale_ = true;
  bool designed_ = false;
  DeEsserTuning applied_;
};

}  // namespace cv
