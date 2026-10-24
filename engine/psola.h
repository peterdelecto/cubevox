#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"

// One pitch-shifted voice by pitch-synchronous overlap-add (Lent 1989 form).
// Grains two periods long are cut on a grid spaced one input period apart
// and laid down one output period apart, so the formants stay put.

namespace cv {

constexpr int kVoiceRingLen = 4800;              // 100 ms shared input ring
using VoiceRing = std::array<float, kVoiceRingLen>;

class PsolaVoice {
 public:
  static constexpr int kGrainDelay = 1040;
  static constexpr int kMaxGrains = 6;  // at ratio <= 2 at most ceil(2r) + 1 = 5 are live

  void reset() {
    grains_.fill(Grain{});
    markDelay_ = grainDelay_;
    countdown_ = 0.0f;
  }

  // Output latency in samples. Each grain reads the input at a fixed delay;
  // kGrainDelay keeps a grain at the longest period behind the write head.
  void setGrainDelay(float samples) { grainDelay_ = clampf(samples, kMinDelay + 1.0f, kGrainDelay); }
  float grainDelay() const { return grainDelay_; }

  // One output sample. writeCount is the ring index of the sample just
  // written, already wrapped below kVoiceRingLen by the owner. period is the
  // sung period in samples (last voiced), ratio the pitch ratio (clamped 0.5..2.0).
  float tick(const VoiceRing& ring, long writeCount, float period, float ratio) {
    if (period <= 0.0f) return 0.0f;
    const float p = clampf(period, kMinPeriod, kMaxPeriod);
    const float r = clampf(ratio, kMinRatio, kMaxRatio);
    const float outPeriod = p / r;
    const int head = static_cast<int>(writeCount);

    advanceMarks(p);
    if (countdown_ > outPeriod) countdown_ = outPeriod;
    countdown_ -= 1.0f;
    if (countdown_ <= 0.0f) {
      launch(head, p, -countdown_);
      countdown_ += outPeriod;
    }

    float sum = 0.0f;
    for (Grain& g : grains_) sum += play(ring, g);
    return sum / r;
  }

 private:
  struct Grain {
    int idx = 0;               // ring index of the next read, advances one per sample
    CatmullRomWeights w{};     // for the fixed fractional offset
    float age = 0.0f;
    float len = 0.0f;
    HannRotor window;    // Hann over len, started at age
    bool active = false;
  };

  static constexpr float kMinPeriod = 48.0f;
  static constexpr float kMaxPeriod = 686.0f;
  static constexpr float kMinRatio = 0.5f;
  static constexpr float kMaxRatio = 2.0f;
  static constexpr float kMinDelay = 3.0f;
  static constexpr float kMaxDelay = kVoiceRingLen - 4.0f;

  static_assert(kGrainDelay >= 1.5f * kMaxPeriod + 4.0f, "grains would cross the write head");

  // Marks are tracked as delays behind the write head, so they stay bounded
  // floats on a box that runs for hours. markDelay_ is the newest mark at or
  // before kGrainDelay.
  void advanceMarks(float p) {
    markDelay_ += 1.0f;
    while (markDelay_ - p >= grainDelay_) markDelay_ -= p;
  }

  // lead is how far past the ideal launch instant this tick already is.
  void launch(int head, float p, float lead) {
    float centre = markDelay_;
    if (centre - grainDelay_ > 0.5f * p) centre -= p;
    Grain* slot = &grains_[0];
    for (Grain& g : grains_) {
      if (!g.active) {
        slot = &g;
        break;
      }
      if (g.age > slot->age) slot = &g;
    }
    // The delay stays constant over the grain, so the fraction is fixed.
    const float delay = clampf(centre + p - lead, kMinDelay, kMaxDelay);
    const float pos = static_cast<float>(head) - delay;
    const float fl = floorf(pos);
    slot->idx = wrap(static_cast<int>(fl));
    slot->w = catmullRomWeights(pos - fl);
    slot->age = lead;
    slot->len = 2.0f * p;
    slot->window.init(kTwoPi * lead / slot->len, kTwoPi / slot->len);
    slot->active = true;
  }

  float play(const VoiceRing& ring, Grain& g) const {
    if (!g.active) return 0.0f;
    if (g.age >= g.len) {
      g.active = false;
      return 0.0f;
    }
    const float w = g.window.window();
    g.window.advance();
    g.age += 1.0f;
    const float y = g.w.m * ring[wrap(g.idx - 1)] + g.w.z * ring[g.idx] +
                    g.w.p * ring[wrap(g.idx + 1)] + g.w.q * ring[wrap(g.idx + 2)];
    g.idx = wrap(g.idx + 1);
    return w * y;
  }

  static int wrap(int i) { return wrapOnce<kVoiceRingLen>(i); }

  std::array<Grain, kMaxGrains> grains_{};
  float grainDelay_ = static_cast<float>(kGrainDelay);
  float markDelay_ = static_cast<float>(kGrainDelay);
  float countdown_ = 0.0f;
};

}  // namespace cv
