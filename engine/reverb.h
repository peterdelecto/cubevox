#pragma once

#include <array>

#include "engine/chasm.h"
#include "engine/common.h"
#include "engine/smooth.h"
#include "engine/spring.h"

// Reverb block: SPRING or CHASM behind one on/off smoother.
// The selected engine runs; the engine left behind is cleared on a switch.
// While settled off, neither engine runs and the block is a bit-exact copy.
// Each engine keeps its own wetDb.

namespace cv {

constexpr int kReverbSpring = 0;
constexpr int kReverbChasm = 1;

struct ReverbParams {
  bool on = true;
  int engine = kReverbSpring;
  SpringParams spring;  // spring.on is ignored; ReverbParams::on rules
  ChasmParams chasm;
};

class Reverb {
 public:
  Reverb() { reset(); }

  void reset() {
    spring_.reset();
    chasm_.reset();
    active_ = 0.0f;
    engine_ = kReverbSpring;
    idle_ = false;
    valid_ = false;
  }

  void process(const float* in, float* out, int n, const ReverbParams& p) {
    const int engine = p.engine == kReverbChasm ? kReverbChasm : kReverbSpring;
    const float target = p.on ? 1.0f : 0.0f;
    if (!valid_) {
      active_ = target;
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
    } else {
      SpringParams sp = p.spring;
      sp.on = true;
      spring_.process(in, wet_.data(), n, sp);
      for (int i = 0; i < n; ++i) wet_[static_cast<size_t>(i)] -= in[i];
    }

    const float a = smooth::coef(smooth::kSmoothSec);
    for (int i = 0; i < n; ++i) {
      active_ = smooth::step(active_, target, a);
      out[i] = in[i] + active_ * wet_[static_cast<size_t>(i)];
    }
  }

  // Test hooks.
  Spring& spring() { return spring_; }
  Chasm& chasm() { return chasm_; }

 private:
  void clear(int engine) {
    if (engine == kReverbChasm)
      chasm_.reset();
    else
      spring_.reset();
  }

  Spring spring_;
  Chasm chasm_;
  std::array<float, kBlock> wet_{};
  float active_ = 0.0f;
  int engine_ = kReverbSpring;
  bool idle_ = false;
  bool valid_ = false;
};

}  // namespace cv
