#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/pitch.h"

// One pitch-shifted voice by pitch-synchronous overlap-add (Lent 1989 form).
// Grains two periods long are cut on a grid spaced one input period apart
// and laid down one output period apart, so the formants stay put.

namespace cv {

// The shared input ring holds the Crowd card's longest lag, the timing scatter its working set rides
// on top of that lag, and the reach of its longest grain (two periods at the tracker's longest, and
// a pad) behind both. The longest lag is a raised copy's, 75 ms behind a tuned copy at the tuned
// copies' 150 ms ceiling. 12,756 samples, 265.75 ms. Autotune reads under 2,100 samples back and
// Octave under 3,850.
constexpr int kCrowdMaxLagMs = 225;
constexpr int kCrowdScatterMs = 12;
constexpr int kCrowdGrainReach = 2 * PitchTracker::kMaxPeriod + 8;
constexpr int kVoiceRingLen = (kCrowdMaxLagMs + kCrowdScatterMs) * (kSampleRate / 1000) + kCrowdGrainReach;
using VoiceRing = std::array<float, kVoiceRingLen>;

// The pitch ratio a PSOLA voice clamps to, and the delays a grain reads at. A grain never reads
// closer to the head than the near bound or further back than the ring holds. The periods a voice
// plays are the tracker's, PitchTracker::kMinPeriod to kMaxPeriod.
constexpr float kMinPitchRatio = 0.5f;
constexpr float kMaxPitchRatio = 2.0f;
constexpr float kMinReadDelay = 3.0f;
constexpr float kMaxReadDelay = kVoiceRingLen - 4.0f;

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
  void setGrainDelay(float samples) { grainDelay_ = clampf(samples, kMinReadDelay + 1.0f, kGrainDelay); }
  float grainDelay() const { return grainDelay_; }

  // One output sample. writeCount is the ring index of the sample just written, already wrapped
  // below kVoiceRingLen by the owner. period is the sung period in samples (last voiced),
  // inverseRatio the inverse of the pitch ratio, which clamps to 0.5..2.0 as the ratio does.
  float tick(const VoiceRing& ring, long writeCount, float period, float inverseRatio) {
    if (period <= 0.0f) return 0.0f;
    const float p = clampf(period, kMinPeriod, kMaxPeriod);
    const float inv = clampf(inverseRatio, 1.0f / kMaxPitchRatio, 1.0f / kMinPitchRatio);
    const float outPeriod = p * inv;
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
    return sum * inv;
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

  static constexpr float kMinPeriod = static_cast<float>(PitchTracker::kMinPeriod);
  static constexpr float kMaxPeriod = static_cast<float>(PitchTracker::kMaxPeriod);

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
    const float delay = clampf(centre + p - lead, kMinReadDelay, kMaxReadDelay);
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
    // One unsigned test keeps the four taps idx - 1 .. idx + 2 inside the ring; only a read across
    // the ring's seam takes the wrapped path.
    const int i = g.idx;
    float y;
    if (static_cast<unsigned>(i - 1) < static_cast<unsigned>(kVoiceRingLen - 3)) {
      const float* r = ring.data() + i;
      y = g.w.m * r[-1] + g.w.z * r[0] + g.w.p * r[1] + g.w.q * r[2];
    } else {
      y = g.w.m * ring[wrap(i - 1)] + g.w.z * ring[i] + g.w.p * ring[wrap(i + 1)] + g.w.q * ring[wrap(i + 2)];
    }
    g.idx = i + 1 == kVoiceRingLen ? 0 : i + 1;
    return w * y;
  }

  static int wrap(int i) { return wrapOnce<kVoiceRingLen>(i); }

  std::array<Grain, kMaxGrains> grains_{};
  float grainDelay_ = static_cast<float>(kGrainDelay);
  float markDelay_ = static_cast<float>(kGrainDelay);
  float countdown_ = 0.0f;
};

}  // namespace cv
