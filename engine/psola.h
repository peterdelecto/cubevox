#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
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

  // Output latency in samples. Each grain reads the input at a fixed delay, so
  // grid PSOLA only needs that delay to stay positive; the default suits every
  // caller that shares the ring with engine B's look-ahead.
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
    int idx = 0;         // ring index of the next read, advances one per sample
    float hm = 0.0f;     // Catmull-Rom weights for the fixed fractional offset
    float h0 = 0.0f;
    float h1 = 0.0f;
    float h2 = 0.0f;
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
  static constexpr float kTwoPi = 6.28318530717958647692f;

  static_assert(kGrainDelay >= 1.5f * kMaxPeriod + 4.0f, "grains would cross the write head");

  static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

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
    const float f = pos - fl;
    const float f2 = f * f;
    const float f3 = f2 * f;
    slot->idx = wrap(static_cast<int>(fl));
    slot->hm = -0.5f * f + f2 - 0.5f * f3;
    slot->h0 = 1.0f - 2.5f * f2 + 1.5f * f3;
    slot->h1 = 0.5f * f + 2.0f * f2 - 1.5f * f3;
    slot->h2 = -0.5f * f2 + 0.5f * f3;
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
    const float y = g.hm * ring[wrap(g.idx - 1)] + g.h0 * ring[g.idx] +
                    g.h1 * ring[wrap(g.idx + 1)] + g.h2 * ring[wrap(g.idx + 2)];
    g.idx = wrap(g.idx + 1);
    return w * y;
  }

  // Reads sit within one ring length of the head, so one step wraps them; the
  // final clamp is never taken and lets the compiler bound the index.
  static int wrap(int i) {
    if (i < 0) i += kVoiceRingLen;
    else if (i >= kVoiceRingLen) i -= kVoiceRingLen;
    return static_cast<unsigned>(i) < static_cast<unsigned>(kVoiceRingLen) ? i : 0;
  }

  std::array<Grain, kMaxGrains> grains_{};
  float grainDelay_ = static_cast<float>(kGrainDelay);
  float markDelay_ = static_cast<float>(kGrainDelay);
  float countdown_ = 0.0f;
};

}  // namespace cv
