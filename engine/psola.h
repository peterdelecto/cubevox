#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"

// One pitch-shifted voice by pitch-synchronous overlap-add (Lent 1989 form).
// Grains two periods long are cut on a grid spaced one input period apart
// and laid down one output period apart, so the formants stay put.

namespace cv {

constexpr int kVoiceRingLen = 4800;              // 100 ms shared input ring
using VoiceRing = std::array<float, kVoiceRingLen>;

class PsolaVoice {
 public:
  static constexpr int kGrainDelay = 1040;
  static constexpr int kMaxGrains = 8;

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

  // One output sample. writeCount is the absolute index of the sample just
  // written at ring[writeCount % kVoiceRingLen]. period is the sung period in
  // samples (last voiced), ratio the pitch ratio (clamped 0.5..2.0).
  float tick(const VoiceRing& ring, long writeCount, float period, float ratio) {
    if (period <= 0.0f) return 0.0f;
    const float p = clampf(period, kMinPeriod, kMaxPeriod);
    const float r = clampf(ratio, kMinRatio, kMaxRatio);
    const float outPeriod = p / r;
    const int head = static_cast<int>(writeCount % kVoiceRingLen);

    advanceMarks(p);
    if (countdown_ > outPeriod) countdown_ = outPeriod;
    countdown_ -= 1.0f;
    if (countdown_ <= 0.0f) {
      launch(p, -countdown_);
      countdown_ += outPeriod;
    }

    float sum = 0.0f;
    for (Grain& g : grains_) sum += play(ring, head, g);
    return sum / r;
  }

 private:
  struct Grain {
    float delay = 0.0f;  // constant distance behind the write head
    float age = 0.0f;
    float len = 0.0f;
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
  void launch(float p, float lead) {
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
    slot->delay = clampf(centre + p - lead, kMinDelay, kMaxDelay);
    slot->age = lead;
    slot->len = 2.0f * p;
    slot->active = true;
  }

  float play(const VoiceRing& ring, int head, Grain& g) const {
    if (!g.active) return 0.0f;
    if (g.age >= g.len) {
      g.active = false;
      return 0.0f;
    }
    const float w = 0.5f * (1.0f - cosf(kTwoPi * g.age / g.len));
    g.age += 1.0f;
    return w * readCubic(ring, head, g.delay);
  }

  static int wrap(int i) {
    i %= kVoiceRingLen;
    return i < 0 ? i + kVoiceRingLen : i;
  }

  // Catmull-Rom at a fractional delay behind the newest sample.
  static float readCubic(const VoiceRing& ring, int head, float delay) {
    const float pos = static_cast<float>(head) - delay;
    const float fl = floorf(pos);
    const float f = pos - fl;
    const int i0 = wrap(static_cast<int>(fl));
    const float xm = ring[wrap(i0 - 1)];
    const float x0 = ring[i0];
    const float x1 = ring[wrap(i0 + 1)];
    const float x2 = ring[wrap(i0 + 2)];
    const float c1 = 0.5f * (x1 - xm);
    const float c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return x0 + f * (c1 + f * (c2 + f * c3));
  }

  std::array<Grain, kMaxGrains> grains_{};
  float grainDelay_ = static_cast<float>(kGrainDelay);
  float markDelay_ = static_cast<float>(kGrainDelay);
  float countdown_ = 0.0f;
};

}  // namespace cv
