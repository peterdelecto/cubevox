#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"

// Unison: two copies of the voice, each with a slow LFO wobble on its delay
// (chorus) and a fixed detune from a crossfaded dual-tap shifter (doubler).
// One DEPTH knob scales swing, fixed detune, and wet level together, so the
// two flavours blend through the tuning constants, not separate controls.

namespace cv {

struct UnisonTuning {
  // Owner-tuned by ear 2026-10-01 against dry vocal loops.
  float baseDelayMs[2] = {20.0f, 15.0f};
  float lfoHz[2] = {0.60f, 0.90f};
  float swingMinMs = 0.5f;   // modulation swing (peak) at DEPTH 0
  float swingMaxMs = 3.0f;   // modulation swing (peak) at DEPTH 1
  float wetMaxDb = 0.0f;    // per-voice level at DEPTH 1
  float detuneCents[2] = {2.0f, -2.0f};  // fixed per-voice detune at DEPTH 1
  float windowMs = 20.0f;    // crossfade window of the dual-tap shifter
};

struct UnisonParams {
  bool on = false;
  float depth = 0.0f;  // 0..1
  UnisonTuning tuning;
};

class Unison {
 public:
  void reset() {
    line_.fill(0.0f);
    phase_ = {0.0f, 0.0f};
    shiftPhase_ = {0.0f, 0.0f};
    depth_ = 0.0f;
    writePos_ = 0;
  }

  // Test hook: mute one voice's wet contribution.
  void setVoiceEnabled(int v, bool on) { enabled_[v] = on; }

  void process(const float* in, float* out, int n, const UnisonParams& p) {
    const UnisonTuning& t = p.tuning;
    const float target = p.on ? clamp01(p.depth) : 0.0f;
    const float wetMax = powf(10.0f, t.wetMaxDb / 20.0f);
    const float smooth = 1.0f - expf(-1.0f / (kSmoothSec * kSampleRate));
    const float windowSmp = clampWindow(t.windowMs) * kSmpPerMs;
    float inc[2];
    for (int v = 0; v < 2; ++v) inc[v] = kTwoPi * t.lfoHz[v] / kSampleRate;

    for (int i = 0; i < n; ++i) {
      stepDepth(target, smooth);
      const float swing = t.swingMinMs + (t.swingMaxMs - t.swingMinMs) * depth_;
      const float wetGain = depth_ * wetMax;

      line_[writePos_] = in[i];

      float wet = 0.0f;
      for (int v = 0; v < 2; ++v) {
        const float centre = t.baseDelayMs[v] * kSmpPerMs + swing * kSmpPerMs * sinf(phase_[v]);
        const float s = readShifted(v, centre, windowSmp);
        if (enabled_[v]) wet += s;
        phase_[v] += inc[v];
        if (phase_[v] >= kTwoPi) phase_[v] -= kTwoPi;
        // Pitch ratio r moves the read head at r x write speed, so the tap's
        // delay drifts by (1 - r) per sample; the sawtooth is that drift
        // normalised to the window.
        const float ratio = powf(2.0f, t.detuneCents[v] * depth_ / 1200.0f);
        shiftPhase_[v] = frac(shiftPhase_[v] + (1.0f - ratio) / windowSmp);
      }

      out[i] = in[i] + wetGain * wet;
      writePos_ = (writePos_ + 1) % kLen;
    }
  }

 private:
  static constexpr int kDelayMs = 80;
  static constexpr int kLen = kDelayMs * kSampleRate / 1000;
  static constexpr float kSmpPerMs = kSampleRate / 1000.0f;
  static constexpr float kWindowMinMs = 5.0f;
  static constexpr float kWindowMaxMs = 30.0f;
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr float kSmoothSec = 0.020f;
  static constexpr float kSnapBelow = 1e-6f;

  static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
  static float clampWindow(float ms) {
    return ms < kWindowMinMs ? kWindowMinMs : (ms > kWindowMaxMs ? kWindowMaxMs : ms);
  }
  static float frac(float x) {
    x -= floorf(x);
    return x < 0.0f ? x + 1.0f : x;
  }

  // Two taps half a window apart on a sawtooth, Hann-crossfaded so the sum
  // of the gains is always 1. Taps sit at centre + window * (p - 0.5), so
  // with the phase parked at 0 the live tap is exactly at centre and the
  // output matches a plain single-tap read.
  float readShifted(int v, float centre, float windowSmp) const {
    float sum = 0.0f;
    for (int k = 0; k < 2; ++k) {
      const float p = frac(shiftPhase_[v] + 0.5f * k);
      const float gain = 0.5f * (1.0f - cosf(kTwoPi * p));
      if (gain <= 0.0f) continue;
      sum += gain * readCubic(centre + windowSmp * (p - 0.5f));
    }
    return sum;
  }

  // One-pole toward target; snaps to exact 0 so settled depth 0 adds nothing.
  void stepDepth(float target, float a) {
    depth_ += a * (target - depth_);
    if (target == 0.0f && depth_ < kSnapBelow) depth_ = 0.0f;
  }

  static int wrap(int i) {
    if (i < 0) return i + kLen;
    if (i >= kLen) return i - kLen;
    return i;
  }

  // Catmull-Rom through four taps. Delay is clamped so the newest tap
  // never crosses the write head and the oldest never wraps onto it.
  float readCubic(float delay) const {
    const float d = delay < 2.0f ? 2.0f : (delay > kLen - 3.0f ? kLen - 3.0f : delay);
    const float pos = static_cast<float>(writePos_) - d;
    const float fl = floorf(pos);
    const float f = pos - fl;
    const int i0 = wrap(static_cast<int>(fl) % kLen);
    const float xm = line_[wrap(i0 - 1)];
    const float x0 = line_[i0];
    const float x1 = line_[wrap(i0 + 1)];
    const float x2 = line_[wrap(i0 + 2)];
    const float c1 = 0.5f * (x1 - xm);
    const float c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return x0 + f * (c1 + f * (c2 + f * c3));
  }

  // Both voices read the same input, so one delay line serves both.
  std::array<float, kLen> line_{};
  std::array<float, 2> phase_{{0.0f, 0.0f}};
  std::array<float, 2> shiftPhase_{{0.0f, 0.0f}};
  std::array<bool, 2> enabled_{{true, true}};
  float depth_ = 0.0f;
  int writePos_ = 0;
};

}  // namespace cv
