#pragma once

#include <array>
#include <cmath>

#include "engine/chasm.h"
#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/reverb_parts.h"
#include "engine/smooth.h"
#include "engine/spring.h"

// Reverb block: Spring or Chasm behind one on/off smoother.
// The selected engine runs; the engine left behind is cleared on a switch.
// While settled off, neither engine runs and the block is a bit-exact copy.
// The engines' loops carry bass cuts, so the wet path holds no DC and needs no blocker (reverb_clean_test
// holds it under -90 dBFS). MIX 0 leaves the dry path untouched, a bit-exact copy.
// Each engine keeps its own wetDb, a level trim. The crossfade runs equal-power
// from dry (0) to wet only (1), smoothed over 20 ms; its gains are evaluated at
// the block edges and interpolated between.

namespace cv {

constexpr int kReverbSpring = 0;
constexpr int kReverbChasm = 1;

// MIX sets the crossfade and TIME the decays, each piecewise linear through
// 0 / 0.5 / 1. 0.5 lands on the default crossfade and decays. Chasm's TIME curve is kChasmFeedback.
using reverb_parts::KnobCurve;
constexpr KnobCurve kSpringDecay{0.25f, 0.50f, 0.85f};
constexpr KnobCurve kReverbWet{0.0f, 0.15f, 0.60f};

struct ReverbParams {
  bool on = true;
  int engine = kReverbSpring;
  ChasmParams chasm;
  SpringParams spring;
  float mix = 0.5f;   // panel MIX 0..1; applyKnobs sets wet from it
  float time = 0.5f;  // panel TIME 0..1; applyKnobs sets every engine's decay from it
  float wet = kReverbWet.mid;  // crossfade, 0 dry .. 1 wet only
};

// The struct defaults are the knobs' middle position, so an engine run without applyKnobs sounds as
// the box does at 12 o'clock.
static_assert(ChasmParams{}.feedback == kChasmFeedback.mid && SpringParams{}.decay == kSpringDecay.mid &&
                  ReverbParams{}.mix == 0.5f && ReverbParams{}.time == 0.5f,
              "the engine defaults are the knob curves' middles");

inline void applyKnobs(ReverbParams& r) {
  r.wet = kReverbWet.at(r.mix);
  r.chasm.feedback = kChasmFeedback.at(r.time);
  r.spring.decay = kSpringDecay.at(r.time);
}

class Reverb {
 public:
  Reverb() { reset(); }

  // Clears the engine that holds state. A switch and an idle block clear the engine they leave, so
  // the other one is clear already.
  void reset() {
    clear(engine_);
    active_ = 0.0f;
    wet_ = 0.0f;
    gains_ = gainsAt(0.0f);
    engine_ = kReverbSpring;
    idle_ = false;
    valid_ = false;
  }

  // Reverb's scratch holds one block, so a longer call runs in block-sized pieces.
  void process(const float* in, float* out, int n, const ReverbParams& p) {
    for (int done = 0; done < n; done += kBlock) {
      const int m = n - done < kBlock ? n - done : kBlock;
      processBlock(in + done, out + done, m, p);
    }
  }

  // Test hooks.
  Chasm& chasm() { return chasm_; }
  Spring& spring() { return spring_; }

 private:
  struct Gains {
    float dry, wet;
  };

  static Gains gainsAt(float wet) {
    if (wet <= 0.0f) return {1.0f, 0.0f};
    return {cosf(wet * kHalfPi), sinf(wet * kHalfPi)};
  }

  void processBlock(const float* in, float* out, int n, const ReverbParams& p) {
    const int engine = p.engine == kReverbChasm ? kReverbChasm : kReverbSpring;
    const float target = p.on ? 1.0f : 0.0f;
    const float wetTarget = smooth::clamp01(p.wet);
    if (!valid_) {
      active_ = target;
      wet_ = wetTarget;
      gains_ = gainsAt(wet_);
      engine_ = engine;
      valid_ = true;
    }
    if (engine != engine_) {
      clear(engine_);
      engine_ = engine;
    }
    if (active_ == 0.0f && target == 0.0f) {
      if (!idle_) clear(engine_);
      idle_ = true;
      for (int i = 0; i < n; ++i) out[i] = in[i];
      return;
    }
    idle_ = false;
    if (engine_ == kReverbChasm) {
      chasm_.process(in, scratch_.data(), n, p.chasm);
    } else {
      spring_.process(in, scratch_.data(), n, p.spring);
    }
    mixBlock(in, out, n, target, wetTarget);
  }

  void mixBlock(const float* in, float* out, int n, float target, float wetTarget) {
    // A smoother at its target holds it, and advance returns at once for one that sits there.
    const float wetEnd = smooth::advance(wet_, wetTarget, smooth::kSmoothCoef, n);
    const Gains g0 = gains_;
    if (wetEnd != wet_) gains_ = gainsAt(wetEnd);
    const Gains g1 = gains_;
    if (active_ == target && wetEnd == wet_) {
      const float dryLess1 = g0.dry - 1.0f;
      for (int i = 0; i < n; ++i)
        out[i] = in[i] + active_ * (dryLess1 * in[i] + g0.wet * scratch_[static_cast<size_t>(i)]);
      return;
    }
    const float inv = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      active_ = smooth::step(active_, target, smooth::kSmoothCoef);
      const float f = static_cast<float>(i + 1) * inv;
      const float dry = g0.dry + (g1.dry - g0.dry) * f;
      const float wet = g0.wet + (g1.wet - g0.wet) * f;
      out[i] = in[i] + active_ * ((dry - 1.0f) * in[i] + wet * scratch_[static_cast<size_t>(i)]);
    }
    wet_ = wetEnd;
  }

  void clear(int engine) {
    if (engine == kReverbChasm)
      chasm_.reset();
    else
      spring_.reset();
  }

  Chasm chasm_;
  Spring spring_;
  std::array<float, kBlock> scratch_{};  // the engine's wet output
  float active_ = 0.0f;
  float wet_ = 0.0f;  // smoothed crossfade
  Gains gains_{1.0f, 0.0f};  // crossfade gains at wet_
  int engine_ = kReverbSpring;
  bool idle_ = false;
  bool valid_ = false;
};

}  // namespace cv
