#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/fastmath.h"

// Unison: two copies of the voice, each with a slow LFO wobble on its delay
// (chorus) and a fixed detune from a crossfaded dual-tap shifter (doubler).
// One DEPTH knob scales swing, fixed detune, and wet level together, so the
// two flavours blend through the tuning constants, not separate controls.

namespace cv {

struct UnisonTuning {
  // Owner-tuned by ear 2026-10-01 against dry vocal loops.
  float baseDelayMs[2] = {20.0f, 15.0f};
  float lfoHz[2] = {0.20f, 0.30f};
  float swingMinMs = 1.0f;   // modulation swing (peak) at DEPTH 0
  float swingMaxMs = 6.0f;   // modulation swing (peak) at DEPTH 1
  float wetMaxDb = -4.0f;   // per-voice level at DEPTH 1
  float detuneCents[2] = {0.0f, 0.0f};  // fixed per-voice detune at DEPTH 1
  float windowMs = 20.0f;    // crossfade window of the dual-tap shifter
  // Whole-output gain (dry + wet) at DEPTH 1, scaled by depth in dB; level rule at DEPTH 0.8.
  float trimDb = -1.4f;
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
    lfoPhase_ = {0u, 0u};
    shiftPhase_ = {0u, 0u};
    lfoInc_ = {0u, 0u};
    shiftInc_ = {0u, 0u};
    depth_ = 0.0f;
    swingSmp_ = 0.0f;
    writePos_ = 0;
  }

  // Test hook: mute one voice's wet contribution.
  void setVoiceEnabled(int v, bool on) { enabled_[v] = on; }

  // Test hooks: LFO phase in turns of 2^32 and the last swing in samples.
  uint32_t lfoPhase(int v) const { return lfoPhase_[v]; }
  float swingSamples() const { return swingSmp_; }

  void process(const float* in, float* out, int n, const UnisonParams& p) {
    const UnisonTuning& t = p.tuning;
    const float target = p.on ? clamp01(p.depth) : 0.0f;
    const float wetMax = powf(10.0f, t.wetMaxDb / 20.0f);
    const float smooth = 1.0f - expf(-1.0f / (kSmoothSec * kSampleRate));
    const float windowSmp = clampWindow(t.windowMs) * kSmpPerMs;

    for (int v = 0; v < 2; ++v) {
      const float hz = t.lfoHz[v] < 0.0f ? 0.0f : (t.lfoHz[v] > kLfoMaxHz ? kLfoMaxHz : t.lfoHz[v]);
      lfoInc_[v] = static_cast<uint32_t>(hz / kSampleRate * kTurn + 0.5f);
    }

    // Off and settled: only the line and the LFO phases advance.
    if (depth_ == 0.0f && target == 0.0f) {
      for (int i = 0; i < n; ++i) {
        line_[writePos_] = in[i];
        out[i] = in[i];
        writePos_ = writePos_ + 1 == kLen ? 0 : writePos_ + 1;
      }
      for (int v = 0; v < 2; ++v) lfoPhase_[v] += lfoInc_[v] * static_cast<uint32_t>(n);
      return;
    }

    // Depth steps per sample; ratio and trim depend on it alone, so they are
    // evaluated at the block edges and the trim is interpolated between.
    float depthEnd = depth_;
    for (int i = 0; i < n; ++i) depthEnd = stepped(depthEnd, target, smooth);
    const float trim0 = powf(10.0f, t.trimDb * depth_ / 20.0f);
    const float trim1 = powf(10.0f, t.trimDb * depthEnd / 20.0f);
    const float inv = 1.0f / static_cast<float>(n);

    // Pitch ratio r moves the read head at r x write speed, so the tap's
    // delay drifts by (1 - r) per sample; the sawtooth is that drift
    // normalised to the window.
    for (int v = 0; v < 2; ++v) {
      const float ratio = powf(2.0f, t.detuneCents[v] * depthEnd / 1200.0f);
      const float turns = (1.0f - ratio) / windowSmp;
      shiftInc_[v] =
          static_cast<uint32_t>(static_cast<int64_t>(llroundf(turns * kTurn)));
    }

    // Swing and wet gain follow the depth ramp linearly across the block.
    const float depth0 = depth_;
    const float swing0 = (t.swingMinMs + (t.swingMaxMs - t.swingMinMs) * depth0) * kSmpPerMs;
    const float swing1 = (t.swingMinMs + (t.swingMaxMs - t.swingMinMs) * depthEnd) * kSmpPerMs;
    const float wet0 = depth0 * wetMax;
    const float wet1 = depthEnd * wetMax;
    const float base0 = t.baseDelayMs[0];
    const float base1 = t.baseDelayMs[1];
    const float en0 = enabled_[0] ? 1.0f : 0.0f;
    const float en1 = enabled_[1] ? 1.0f : 0.0f;

    const float* line = line_.data();
    float* wline = line_.data();
    int wp = writePos_;
    uint32_t lfo0 = lfoPhase_[0];
    uint32_t lfo1 = lfoPhase_[1];
    const uint32_t lfoInc0 = lfoInc_[0];
    const uint32_t lfoInc1 = lfoInc_[1];
    uint32_t sh0 = shiftPhase_[0];
    uint32_t sh1 = shiftPhase_[1];
    const uint32_t shInc0 = shiftInc_[0];
    const uint32_t shInc1 = shiftInc_[1];

    if (sh0 == 0u && shInc0 == 0u && sh1 == 0u && shInc1 == 0u) {
      for (int i = 0; i < n; ++i) {
        const float x = in[i];
        const float f = static_cast<float>(i + 1) * inv;
        const float swing = swing0 + (swing1 - swing0) * f;
        const float wetGain = wet0 + (wet1 - wet0) * f;

        wline[wp] = x;

        const float c0 = base0 * kSmpPerMs + swing * sinTurns(lfo0);
        const float c1 = base1 * kSmpPerMs + swing * sinTurns(lfo1);
        const float s0 = readCubic(line, wp, c0);
        const float s1 = readCubic(line, wp, c1);
        const float wet = (0.0f + s0 * en0) + s1 * en1;
        lfo0 += lfoInc0;
        lfo1 += lfoInc1;

        out[i] = (x + wetGain * wet) * (trim0 + (trim1 - trim0) * f);
        wp = wp + 1 == kLen ? 0 : wp + 1;
      }
    } else {
      for (int i = 0; i < n; ++i) {
        const float x = in[i];
        const float f = static_cast<float>(i + 1) * inv;
        const float swing = swing0 + (swing1 - swing0) * f;
        const float wetGain = wet0 + (wet1 - wet0) * f;

        wline[wp] = x;

        const float c0 = base0 * kSmpPerMs + swing * sinTurns(lfo0);
        const float c1 = base1 * kSmpPerMs + swing * sinTurns(lfo1);
        const float s0 = readShifted(line, wp, sh0, shInc0, c0, windowSmp);
        const float s1 = readShifted(line, wp, sh1, shInc1, c1, windowSmp);
        const float wet = (0.0f + s0 * en0) + s1 * en1;
        lfo0 += lfoInc0;
        lfo1 += lfoInc1;
        sh0 += shInc0;
        sh1 += shInc1;

        out[i] = (x + wetGain * wet) * (trim0 + (trim1 - trim0) * f);
        wp = wp + 1 == kLen ? 0 : wp + 1;
      }
    }

    writePos_ = wp;
    lfoPhase_[0] = lfo0;
    lfoPhase_[1] = lfo1;
    shiftPhase_[0] = sh0;
    shiftPhase_[1] = sh1;
    depth_ = depthEnd;
    swingSmp_ = swing1;
  }

 private:
  static constexpr int kDelayMs = 80;
  static constexpr int kLen = kDelayMs * kSampleRate / 1000;
  static constexpr float kSmpPerMs = kSampleRate / 1000.0f;
  static constexpr float kWindowMinMs = 5.0f;
  static constexpr float kWindowMaxMs = 30.0f;
  static constexpr float kLfoMaxHz = 20.0f;
  static constexpr float kTurn = 4294967296.0f;
  static constexpr float kSmoothSec = 0.020f;
  static constexpr float kSnapBelow = 1e-6f;

  static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
  static float clampWindow(float ms) {
    return ms < kWindowMinMs ? kWindowMinMs : (ms > kWindowMaxMs ? kWindowMaxMs : ms);
  }

  // Parked shifter (phase and step both 0) is a single tap at centre. Else
  // two taps half a window apart on a sawtooth, Hann-crossfaded with gains
  // sin^2(pi p) and 1 - sin^2(pi p), which sum to exactly 1. Taps sit at
  // centre + window * (p - 0.5).
  static float readShifted(const float* line, int wp, uint32_t ph, uint32_t inc, float centre,
                           float windowSmp) {
    if (ph == 0u && inc == 0u) return readCubic(line, wp, centre);
    const float p0 = static_cast<float>(ph) * (1.0f / kTurn);
    const float p1 = static_cast<float>(ph + 0x80000000u) * (1.0f / kTurn);
    const float s = sinTurns(ph >> 1);
    const float g0 = s * s;
    const float g1 = 1.0f - g0;
    float sum = 0.0f;
    if (g0 > 0.0f) sum += g0 * readCubic(line, wp, centre + windowSmp * (p0 - 0.5f));
    if (g1 > 0.0f) sum += g1 * readCubic(line, wp, centre + windowSmp * (p1 - 0.5f));
    return sum;
  }

  // One-pole toward target; snaps to exact 0 so settled depth 0 adds nothing.
  static float stepped(float d, float target, float a) {
    d += a * (target - d);
    if (target == 0.0f && d < kSnapBelow) d = 0.0f;
    return d;
  }

  // Reads sit within one ring length of the head, so one step wraps them; the
  // final clamp is never taken and lets the compiler bound the index.
  static int wrap(int i) {
    if (i < 0) i += kLen;
    else if (i >= kLen) i -= kLen;
    return static_cast<unsigned>(i) < static_cast<unsigned>(kLen) ? i : 0;
  }

  // Catmull-Rom through four taps. Delay is clamped so the newest tap
  // never crosses the write head and the oldest never wraps onto it.
  static float readCubic(const float* line, int wp, float delay) {
    const float d = delay < 2.0f ? 2.0f : (delay > kLen - 3.0f ? kLen - 3.0f : delay);
    const float pos = static_cast<float>(wp) - d;
    const float fl = floorf(pos);
    const float f = pos - fl;
    const int i0 = wrap(static_cast<int>(fl));
    const float xm = line[wrap(i0 - 1)];
    const float x0 = line[i0];
    const float x1 = line[wrap(i0 + 1)];
    const float x2 = line[wrap(i0 + 2)];
    const float c1 = 0.5f * (x1 - xm);
    const float c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return x0 + f * (c1 + f * (c2 + f * c3));
  }

  // Both voices read the same input, so one delay line serves both.
  std::array<float, kLen> line_{};
  std::array<uint32_t, 2> lfoPhase_{{0u, 0u}};
  std::array<uint32_t, 2> shiftPhase_{{0u, 0u}};
  std::array<uint32_t, 2> lfoInc_{{0u, 0u}};
  std::array<uint32_t, 2> shiftInc_{{0u, 0u}};
  std::array<bool, 2> enabled_{{true, true}};
  float depth_ = 0.0f;
  float swingSmp_ = 0.0f;
  int writePos_ = 0;
};

}  // namespace cv
