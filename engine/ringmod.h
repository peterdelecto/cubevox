#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/pitch.h"
#include "engine/pitch_fx.h"
#include "engine/smooth.h"

namespace cv {

// The Ring mod card. A sine carrier at RATIO times the tracked pitch multiplies the voice, so the clang
// stays in tune and follows the melody. The carrier is a phase counter read through sinTurns. Its
// frequency is set once per block from the tracker's period and glides over 10 ms. Unvoiced frames hold
// the last frequency, and before the first voiced frame the carrier sits at 220 Hz times RATIO.
// MIX is an equal-power crossfade, and the wet alone is trimmed to unity.

constexpr int kRingModRatioCount = 7;
constexpr std::array<float, kRingModRatioCount> kRingModRatio = {0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f};
constexpr std::array<const char*, kRingModRatioCount> kRingModRatioName = {"X0.5", "X1",   "X1.5", "X2",
                                                                            "X2.5", "X3",   "X4"};
constexpr uint8_t kRingModDefaultRatioStep = 2;

struct RingModTuning {
  float glideMs = 10.0f;    // the carrier's frequency follows the pitch with this time constant
  float restHz = 220.0f;    // the pitch the carrier assumes before the first voiced frame
  float trimDb = 3.15f;     // the wet path's gain; a sine carrier alone leaves the voice 3.01 dB down
};

struct RingModParams {
  bool on = false;
  float ratio = 1.5f;  // knob A (RATIO), the carrier's frequency over the pitch
  float mix = 0.5f;    // knob B (MIX), 0..1
  RingModTuning tuning;
};

namespace ringmod_detail {

// The carrier stays under this many hertz so its step never reaches half the sample rate.
constexpr float kMaxCarrierHz = 0.45f * kSampleRate;

// The dry-to-card crossfade takes 20 ms, in a straight line.
constexpr float kFadeStep = 1.0f / (20.0f * kSmpPerMs);

// One quarter turn in the phase units of sinTurns, the equal-power law's span.
constexpr uint32_t kQuarterTurn = 1u << 30;

inline float toward(float x, float target, float step) {
  if (x < target) return target - x <= step ? target : x + step;
  return x - target <= step ? target : x - step;
}

}  // namespace ringmod_detail

class RingMod {
 public:
  RingMod() { reset(); }

  void reset() {
    phase_ = 0;
    heldHz_ = 0.0f;
    carrierHz_ = 0.0f;
    mix_ = 0.0f;
    wet_ = 0.0f;
    fresh_ = true;
  }

  // `pitch` has run this block. Off and settled, out equals in bit for bit. in and out may be the same
  // buffer.
  void process(const float* in, float* out, int n, const RingModParams& p, const PitchFx& pitch) {
    if (n <= 0) return;
    const PitchResult& pr = pitch.pitch();
    if (pr.voiced && pr.period > 0.0f) heldHz_ = kSampleRate / PitchTracker::clampPeriod(pr.period);
    const float wetTarget = p.on ? 1.0f : 0.0f;
    if (wet_ == 0.0f && wetTarget == 0.0f) {
      fresh_ = true;
      if (out != in) std::memmove(out, in, sizeof(float) * static_cast<size_t>(n));
      return;
    }

    // The carrier's frequency: a jump on the first call, a 10 ms glide after.
    const float baseHz = heldHz_ > 0.0f ? heldHz_ : p.tuning.restHz;
    const float targetHz = clampf(p.ratio * baseHz, 1.0f, ringmod_detail::kMaxCarrierHz);
    const float hz0 = fresh_ ? targetHz : carrierHz_;
    carrierHz_ = fresh_ ? targetHz : smooth::step(carrierHz_, targetHz, glideCoefFor(p.tuning.glideMs, n));
    const uint32_t step0 = stepOf(hz0);
    const uint32_t step1 = stepOf(carrierHz_);
    const int32_t dStep = static_cast<int32_t>(step1 - step0) / n;

    // MIX glides, and the wet fade scales it, so a switch-on starts at the dry input.
    const float mixKnob = smooth::clamp01(p.mix);
    if (fresh_) mix_ = mixKnob;
    const float e0 = mix_ * wet_;
    mix_ = smooth::step(mix_, mixKnob, smooth::blockCoef(smooth::kSmoothSec, n));
    wet_ = ringmod_detail::toward(wet_, wetTarget, ringmod_detail::kFadeStep * static_cast<float>(n));
    fresh_ = false;
    const float trim = dbToGainFast(clampf(p.tuning.trimDb, -24.0f, 24.0f));
    float dry = dryGain(e0);
    float wet = wetGain(e0) * trim;
    const float inv = 1.0f / static_cast<float>(n);
    const float e1 = mix_ * wet_;
    const float dDry = (dryGain(e1) - dry) * inv;
    const float dWet = (wetGain(e1) * trim - wet) * inv;

    uint32_t phase = phase_;
    uint32_t s = step0;
    for (int i = 0; i < n; ++i) {
      phase += s;
      s += static_cast<uint32_t>(dStep);
      dry += dDry;
      wet += dWet;
      const float x = in[i];
      out[i] = x * (dry + wet * sinTurns(phase));
    }
    phase_ = phase;
  }

  // Test hooks: the carrier's frequency after the block's glide, in Hz.
  float carrierHz() const { return carrierHz_; }

 private:
  static uint32_t stepOf(float hz) { return static_cast<uint32_t>(hz * (4294967296.0f / kSampleRate)); }

  static float dryGain(float e) { return sinTurns(ringmod_detail::kQuarterTurn - phaseOf(e)); }
  static float wetGain(float e) { return sinTurns(phaseOf(e)); }
  static uint32_t phaseOf(float e) {
    return static_cast<uint32_t>(smooth::clamp01(e) * static_cast<float>(ringmod_detail::kQuarterTurn));
  }

  float glideCoefFor(float ms, int n) {
    if (n != coefN_ || ms != coefMs_) {
      coef_ = smooth::blockCoef(ms * 0.001f, n);
      coefN_ = n;
      coefMs_ = ms;
    }
    return coef_;
  }

  uint32_t phase_ = 0;
  float heldHz_ = 0.0f;     // the tracker's pitch, or the last one it gave; 0 before the first
  float carrierHz_ = 0.0f;  // after the glide
  float mix_ = 0.0f;
  float wet_ = 0.0f;        // the dry-to-card fade, 0 dry .. 1 card
  float coef_ = 1.0f;
  float coefMs_ = 0.0f;
  int coefN_ = 0;
  bool fresh_ = true;       // the next call takes the knobs and the frequency outright
};

}  // namespace cv
