#pragma once

#include <array>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"

// Input conditioning: a 2nd-order Butterworth high-pass and a 4th-order Butterworth low-pass at
// the head of the chain, before slot 1. Each filter has an OFF entry that copies bit-exact.
// Coefficients come from ConditioningParams::set on the main loop; process() runs the biquads and
// crossfades from the old filter set to the new one over one block when a cutoff changes. The
// filter state is kept across the change, so the crossfade carries no click.
// Spec: docs/specs/2026-10-30-input-conditioning-design.md.

namespace cv {

// Cutoff lists. Index 0 is OFF.
constexpr std::array<float, 5> kHpfCutoffHz = {0.0f, 80.0f, 100.0f, 120.0f, 150.0f};
constexpr std::array<float, 7> kLpfCutoffHz = {0.0f, 4000.0f, 5000.0f, 6000.0f, 8000.0f, 10000.0f, 12000.0f};
constexpr uint8_t kHpfDefaultIndex = 3;  // 120 Hz
constexpr uint8_t kLpfDefaultIndex = 6;  // 12 kHz

constexpr int kHpfCount = static_cast<int>(kHpfCutoffHz.size());
constexpr int kLpfCount = static_cast<int>(kLpfCutoffHz.size());

// Section Qs of the 4th-order Butterworth: 1 / (2 cos(pi / 8)) and 1 / (2 cos(3 pi / 8)).
constexpr float kLpfQ1 = 0.54119610f;
constexpr float kLpfQ2 = 1.30656296f;

// The cutoffs for one block and the biquad coefficients that realise them. The filter state in the
// biquads stays zero here; the engine keeps its own.
struct ConditioningParams {
  uint8_t hpf = kHpfDefaultIndex;  // index into kHpfCutoffHz
  uint8_t lpf = kLpfDefaultIndex;  // index into kLpfCutoffHz
  Biquad hp;
  std::array<Biquad, 2> lp;

  ConditioningParams() { set(kHpfDefaultIndex, kLpfDefaultIndex); }

  // Takes list indices (an index past its list reads as the last entry) and computes the
  // coefficients. Runs libm, so it belongs on the main loop and not in the audio interrupt.
  void set(uint8_t hpfIndex, uint8_t lpfIndex) {
    hpf = hpfIndex < kHpfCount ? hpfIndex : static_cast<uint8_t>(kHpfCount - 1);
    lpf = lpfIndex < kLpfCount ? lpfIndex : static_cast<uint8_t>(kLpfCount - 1);
    if (hpf != 0) hp.setHighPass(kHpfCutoffHz[hpf], kButterworthQ);
    if (lpf != 0) {
      lp[0].setLowPass(kLpfCutoffHz[lpf], kLpfQ1);
      lp[1].setLowPass(kLpfCutoffHz[lpf], kLpfQ2);
    }
  }
};

class InputConditioning {
 public:
  void reset() {
    cur_.clear();
    old_.clear();
    fadeLeft_ = 0;
  }

  // `in` and `out` may be the same buffer. Takes any `n`.
  void process(const float* in, float* out, int n, const ConditioningParams& p) {
    if (!adopted_) {
      cur_.adopt(p);
      adopted_ = true;
    } else if (p.hpf != cur_.hpf || p.lpf != cur_.lpf) {
      old_ = cur_;
      cur_.adopt(p);
      fadeLeft_ = kFadeSamples;
    }
    if (fadeLeft_ == 0 && !cur_.hpOn() && !cur_.lpOn()) {
      if (out != in) {
        for (int i = 0; i < n; ++i) out[i] = in[i];
      }
      return;
    }
    for (int i = 0; i < n; ++i) {
      const float x = in[i];
      float y = cur_.run(x);
      if (fadeLeft_ > 0) {
        const float w = static_cast<float>(kFadeSamples - fadeLeft_ + 1) * kFadeStep;
        const float yOld = old_.run(x);
        y = yOld + w * (y - yOld);
        --fadeLeft_;
      }
      out[i] = y;
    }
  }

 private:
  // One crossfade lasts a block.
  static constexpr int kFadeSamples = kBlock;
  static constexpr float kFadeStep = 1.0f / static_cast<float>(kFadeSamples);

  // One high-pass and the two low-pass sections, with the indices they were set from.
  struct FilterSet {
    uint8_t hpf = 0;
    uint8_t lpf = 0;
    Biquad hp;
    std::array<Biquad, 2> lp;

    bool hpOn() const { return hpf != 0; }
    bool lpOn() const { return lpf != 0; }

    // Takes the new coefficients and keeps the running state. A filter that turns off clears, so
    // it starts from zero when it turns on again.
    void adopt(const ConditioningParams& p) {
      hpf = p.hpf;
      lpf = p.lpf;
      if (hpOn()) hp.setCoefsFrom(p.hp); else hp.clear();
      if (lpOn()) {
        lp[0].setCoefsFrom(p.lp[0]);
        lp[1].setCoefsFrom(p.lp[1]);
      } else {
        lp[0].clear();
        lp[1].clear();
      }
    }

    void clear() {
      hp.clear();
      lp[0].clear();
      lp[1].clear();
    }

    CV_INLINE float run(float x) {
      if (hpOn()) x = hp.run(x);
      if (lpOn()) x = lp[1].run(lp[0].run(x));
      return x;
    }
  };

  FilterSet cur_;
  FilterSet old_;
  int fadeLeft_ = 0;
  bool adopted_ = false;
};

}  // namespace cv
