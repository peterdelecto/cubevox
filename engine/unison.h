#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Unison: two copies of the voice, each with a slow LFO wobble on its delay
// (chorus). DEPTH scales the swing and the wet level together. A RATE move
// changes the tuning's swing; the swing glides to it with the depth smoother,
// so the taps never jump.

namespace cv {

struct UnisonTuning {
  // Owner-tuned by ear 2026-10-01 against dry vocal loops.
  std::array<float, 2> baseDelayMs = {20.0f, 15.0f};
  std::array<float, 2> lfoHz = {0.20f, 0.30f};
  float swingMinMs = 1.0f;  // modulation swing (peak) at DEPTH 0
  float swingMaxMs = 6.0f;  // modulation swing (peak) at DEPTH 1
  float wetMaxDb = -4.0f;   // per-voice level at DEPTH 1
  // Whole-output gain (dry + wet) at DEPTH 1, scaled by depth in dB; level rule at DEPTH 0.5.
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
    lfoInc_ = {0u, 0u};
    depth_ = 0.0f;
    swingSmp_ = 0.0f;
    writePos_ = 0;
  }

  // Test hook: mute one voice's wet contribution.
  void setVoiceEnabled(int v, bool on) { enabled_[static_cast<size_t>(v)] = on; }

  // Test hooks: LFO phase in turns of 2^32 and the swing at the end of the last block, in samples.
  uint32_t lfoPhase(int v) const { return lfoPhase_[static_cast<size_t>(v)]; }
  float swingSamples() const { return swingSmp_; }

  void process(const float* in, float* out, int n, const UnisonParams& p) {
    const UnisonTuning& t = p.tuning;
    const float target = p.on ? smooth::clamp01(p.depth) : 0.0f;
    setLfoRates(t);

    // Off and settled: only the line and the LFO phases advance, and the swing waits on its
    // depth-0 value so the next engage starts there.
    if (depth_ == 0.0f && target == 0.0f) {
      passThrough(in, out, n);
      swingSmp_ = swingAt(t, 0.0f);
      return;
    }

    // Depth and swing step per sample; trim and wet gain depend on depth alone, so everything is
    // evaluated at the block edges and interpolated between.
    Ramp r;
    r.depth0 = depth_;
    r.swing0 = swingSmp_;
    float depthEnd = depth_;
    float swingEnd = swingSmp_;
    const float swingTarget = swingAt(t, target);
    for (int i = 0; i < n; ++i) {
      depthEnd = smooth::step(depthEnd, target, smooth::kSmoothCoef);
      swingEnd = smooth::step(swingEnd, swingTarget, smooth::kSmoothCoef);
    }
    r.depth1 = depthEnd;
    r.swing1 = swingEnd;
    r.trim0 = powf(10.0f, t.trimDb * r.depth0 / 20.0f);
    r.trim1 = powf(10.0f, t.trimDb * r.depth1 / 20.0f);
    render(in, out, n, t, r);
    depth_ = depthEnd;
    swingSmp_ = swingEnd;
  }

 private:
  static constexpr int kDelayMs = 80;
  static constexpr int kLen = kDelayMs * kSampleRate / 1000;
  static constexpr float kSmpPerMs = kSampleRate / 1000.0f;
  static constexpr float kLfoMaxHz = 20.0f;
  static constexpr float kTurn = 4294967296.0f;
  static constexpr float kClampMargin = 0.01f;

  // Block-edge values of everything that follows the smoothers.
  struct Ramp {
    float depth0 = 0.0f, depth1 = 0.0f;
    float swing0 = 0.0f, swing1 = 0.0f;
    float trim0 = 1.0f, trim1 = 1.0f;
  };

  // LFO swing (peak) in samples at a given depth.
  static float swingAt(const UnisonTuning& t, float depth) {
    return (t.swingMinMs + (t.swingMaxMs - t.swingMinMs) * depth) * kSmpPerMs;
  }

  void setLfoRates(const UnisonTuning& t) {
    for (size_t v = 0; v < 2; ++v) {
      const float hz = clampf(t.lfoHz[v], 0.0f, kLfoMaxHz);
      lfoInc_[v] = static_cast<uint32_t>(hz / kSampleRate * kTurn + 0.5f);
    }
  }

  void passThrough(const float* in, float* out, int n) {
    for (int i = 0; i < n; ++i) {
      line_[static_cast<size_t>(writePos_)] = in[i];
      out[i] = in[i];
      writePos_ = writePos_ + 1 == kLen ? 0 : writePos_ + 1;
    }
    for (size_t v = 0; v < 2; ++v) lfoPhase_[v] += lfoInc_[v] * static_cast<uint32_t>(n);
  }

  // |sinTurns| <= 1 up to rounding, so the tap centres stay within the swing of their base. When
  // that band, with a rounding margin, sits inside the clamp range, the clamp is an identity and
  // the block skips it.
  void render(const float* in, float* out, int n, const UnisonTuning& t, const Ramp& r) {
    constexpr float kLo = 2.0f + kClampMargin;
    constexpr float kHi = kLen - 3.0f - kClampMargin;
    const float swingMax = r.swing0 > r.swing1 ? r.swing0 : r.swing1;
    bool inside = true;
    for (size_t v = 0; v < 2; ++v) {
      const float base = t.baseDelayMs[v] * kSmpPerMs;
      inside = inside && base - swingMax >= kLo && base + swingMax <= kHi;
    }
    const float wetMax = powf(10.0f, t.wetMaxDb / 20.0f);
    const auto unclamped = [](const float* l, int w, float d) { return readCubicUnclamped(l, w, d); };
    const auto clamped = [](const float* l, int w, float d) { return readCubic(l, w, d); };
    if (inside) {
      renderWith(in, out, n, t, r, wetMax, unclamped);
    } else {
      renderWith(in, out, n, t, r, wetMax, clamped);
    }
  }

  // The per-sample loop, with the swing, wet gain and trim ramped linearly across the block.
  template <class Read>
  void renderWith(const float* in, float* out, int n, const UnisonTuning& t, const Ramp& r,
                  float wetMax, const Read& read) {
    const float inv = 1.0f / static_cast<float>(n);
    const float base0 = t.baseDelayMs[0] * kSmpPerMs;
    const float base1 = t.baseDelayMs[1] * kSmpPerMs;
    const float en0 = enabled_[0] ? 1.0f : 0.0f;
    const float en1 = enabled_[1] ? 1.0f : 0.0f;
    float* line = line_.data();
    int wp = writePos_;
    uint32_t lfo0 = lfoPhase_[0];
    uint32_t lfo1 = lfoPhase_[1];
    for (int i = 0; i < n; ++i) {
      const float f = static_cast<float>(i + 1) * inv;
      const float swing = r.swing0 + (r.swing1 - r.swing0) * f;
      const float wetGain = (r.depth0 + (r.depth1 - r.depth0) * f) * wetMax;
      line[wp] = in[i];
      const float s0 = read(line, wp, base0 + swing * sinTurns(lfo0));
      const float s1 = read(line, wp, base1 + swing * sinTurns(lfo1));
      lfo0 += lfoInc_[0];
      lfo1 += lfoInc_[1];
      out[i] = (in[i] + wetGain * (s0 * en0 + s1 * en1)) * (r.trim0 + (r.trim1 - r.trim0) * f);
      wp = wp + 1 == kLen ? 0 : wp + 1;
    }
    writePos_ = wp;
    lfoPhase_[0] = lfo0;
    lfoPhase_[1] = lfo1;
  }

  // Catmull-Rom through four taps. Delay is clamped so the newest tap
  // never crosses the write head and the oldest never wraps onto it.
  static CV_INLINE float readCubic(const float* line, int wp, float delay) {
    return readRing<kLen>(line, wp, clampf(delay, 2.0f, kLen - 3.0f));
  }

  // readCubic for a delay already inside [2, kLen - 3].
  static CV_INLINE float readCubicUnclamped(const float* line, int wp, float d) { return readRing<kLen>(line, wp, d); }

  // Both voices read the same input, so one delay line serves both.
  std::array<float, kLen> line_{};
  std::array<uint32_t, 2> lfoPhase_{{0u, 0u}};
  std::array<uint32_t, 2> lfoInc_{{0u, 0u}};
  std::array<bool, 2> enabled_{{true, true}};
  float depth_ = 0.0f;
  float swingSmp_ = 0.0f;
  int writePos_ = 0;
};

}  // namespace cv
