#pragma once

#include <array>
#include <cmath>

#include "engine/chasm.h"
#include "engine/common.h"
#include "engine/smooth.h"
#include "engine/spring.h"
#include "engine/spring_c.h"

// Reverb block: SPRING, CHASM or PARKER behind one on/off smoother.
// The selected engine runs; the engine left behind is cleared on a switch.
// While settled off, neither engine runs and the block is a bit-exact copy.
// Each engine keeps its own wetDb, a level trim. MIX is an equal-power crossfade
// from dry (0) to wet only (1), smoothed over 20 ms.

namespace cv {

constexpr int kReverbSpring = 0;
constexpr int kReverbChasm = 1;
constexpr int kReverbParker = 2;

struct ReverbParams {
  bool on = true;
  int engine = kReverbSpring;
  SpringParams spring;  // spring.on is ignored; ReverbParams::on rules
  ChasmParams chasm;
  SpringCParams parker;
  float mix = 0.3f;  // panel knob 0 dry .. 1 wet; vocal default (owner 2026-10-01)
};

class Reverb {
 public:
  Reverb() { reset(); }

  void reset() {
    spring_.reset();
    chasm_.reset();
    parker_.reset();
    active_ = 0.0f;
    mix_ = 0.0f;
    dryGain_ = 1.0f;
    wetGain_ = 0.0f;
    engine_ = kReverbSpring;
    idle_ = false;
    valid_ = false;
  }

  void process(const float* in, float* out, int n, const ReverbParams& p) {
    const int engine =
        p.engine == kReverbChasm || p.engine == kReverbParker ? p.engine : kReverbSpring;
    const float target = p.on ? 1.0f : 0.0f;
    const float mixTarget = smooth::clamp01(p.mix);
    if (!valid_) {
      active_ = target;
      mix_ = mixTarget;
      setMixGains();
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
      chasm_.process(in, wet_.data(), n, p.chasm);
    } else if (engine_ == kReverbParker) {
      parker_.process(in, wet_.data(), n, p.parker);
    } else {
      SpringParams sp = p.spring;
      sp.on = true;
      spring_.process(in, wet_.data(), n, sp);
      for (int i = 0; i < n; ++i) wet_[static_cast<size_t>(i)] -= in[i];
    }

    const float a = smooth::coef(smooth::kSmoothSec);
    for (int i = 0; i < n; ++i) {
      active_ = smooth::step(active_, target, a);
      const float prevMix = mix_;
      mix_ = smooth::step(mix_, mixTarget, a);
      if (mix_ != prevMix) setMixGains();
      out[i] = in[i] + active_ * ((dryGain_ - 1.0f) * in[i] + wetGain_ * wet_[static_cast<size_t>(i)]);
    }
  }

  // Test hooks.
  Spring& spring() { return spring_; }
  Chasm& chasm() { return chasm_; }
  SpringC& parker() { return parker_; }

 private:
  static constexpr float kHalfPi = 1.57079632679489661923f;

  void setMixGains() {
    dryGain_ = mix_ <= 0.0f ? 1.0f : cosf(mix_ * kHalfPi);
    wetGain_ = mix_ <= 0.0f ? 0.0f : sinf(mix_ * kHalfPi);
  }

  void clear(int engine) {
    if (engine == kReverbChasm)
      chasm_.reset();
    else if (engine == kReverbParker)
      parker_.reset();
    else
      spring_.reset();
  }

  Spring spring_;
  Chasm chasm_;
  SpringC parker_;
  std::array<float, kBlock> wet_{};
  float active_ = 0.0f;
  float mix_ = 0.0f;
  float dryGain_ = 1.0f;
  float wetGain_ = 0.0f;
  int engine_ = kReverbSpring;
  bool idle_ = false;
  bool valid_ = false;
};

}  // namespace cv
