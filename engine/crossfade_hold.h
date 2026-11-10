#pragma once

#include <cmath>
#include <limits>

#include "engine/dsp.h"

// Holds a pitch card's crossfade at the power its law intends while the crossfade moves.
//
// A card crossfades its input against a voice that plays a grain delay later. A crossfade law assumes how the
// two sides add: Octave's equal-power pair assumes they are unrelated, Formant's linear pair assumes they
// match. On a steady note the voice can sit in phase or out of phase with the input, so the sum swells or
// cancels mid-fade: Formant's switch-on dipped 9 dB at 98 Hz, and Octave's step through SEMITONES 0 dipped
// 4.9 dB by where the grain window sat. The caller hands each sample of a moving crossfade the power its law
// intends (`want`) and the sum (`y`). Running powers over about 1.3 ms give the gain that turns one into the
// other, held between kMinGain and kMaxGain and worked out every kEvery samples of the stream. The gain glides
// through two one-poles, so its slope has no corner. Everything runs per sample, so any call length gives the
// same output. A settled crossfade glides back to 1 and then costs nothing.

namespace cv {

class CrossfadeHold {
 public:
  static constexpr float kMinGain = 0.5f;  // -6 dB
  static constexpr float kMaxGain = 4.0f;  // +12 dB

  // Gain 1 and the running powers clear. The stream's sample count carries on.
  void reset() {
    want_ = 0.0f;
    got_ = 0.0f;
    target_ = 1.0f;
    lead_ = 1.0f;
    gain_ = 1.0f;
  }

  // True while the gain sits at 1 with nothing to glide back from.
  bool idle() const { return gain_ == 1.0f && lead_ == 1.0f && target_ == 1.0f; }

  // One sample of a moving crossfade. `weight`, 0 to 1, is how far the held gain stands in for 1.
  float hold(float want, float y, float weight = 1.0f) {
    want_ += kPowerCoef * (want - want_);
    got_ += kPowerCoef * (y * y - got_);
    if (++count_ >= kEvery) {
      count_ = 0;
      target_ = 1.0f + weight * (gainNow() - 1.0f);
    }
    return glide() * y;
  }

  // One sample of a settled crossfade: the running powers clear and the gain glides back to 1.
  float release(float y) {
    if (idle()) return y;
    want_ = 0.0f;
    got_ = 0.0f;
    target_ = 1.0f;
    if (++count_ >= kEvery) count_ = 0;
    const float g = glide();
    if (fabsf(lead_ - 1.0f) < kSettle && fabsf(gain_ - 1.0f) < kSettle) {
      lead_ = 1.0f;
      gain_ = 1.0f;
    }
    return g * y;
  }

  // n settled samples of a stream that needs no gain, for a caller's fast path. Keeps the stream's count.
  void pass(int n) { count_ = (count_ + n) % kEvery; }

 private:
  static constexpr float kPowerCoef = 1.0f / 64.0f;  // running powers' one-pole, about 1.3 ms
  static constexpr float kGlide = 1.0f / 32.0f;      // each of the gain's two one-poles
  static constexpr int kEvery = 16;
  static constexpr float kFloor = 1e-20f;   // below this a power is silence
  static constexpr float kSettle = 1e-4f;   // a gain this near 1 with the crossfade settled lands on 1
  // The hold leaves the first half dB either way alone and corrects what is past it. A hold that chases
  // every ripple of the running powers bent a 220 Hz sine 1.7 % harder than steady state through a switch-on.
  static constexpr float kSlack = 1.06f;

  // The gain the running powers ask for. Silence asks for 1. A NaN or an infinity in the powers, which no
  // later sample would wash out, clears them.
  float gainNow() {
    const float big = std::numeric_limits<float>::max();
    if (!(got_ > kFloor && want_ > kFloor && got_ <= big && want_ <= big)) {
      if (!(got_ >= 0.0f && got_ <= big && want_ >= 0.0f && want_ <= big)) {
        want_ = 0.0f;
        got_ = 0.0f;
      }
      return 1.0f;
    }
    const float g = sqrtf(want_ / got_);
    const float past = g > kSlack ? g * (1.0f / kSlack) : (g * kSlack < 1.0f ? g * kSlack : 1.0f);
    return clampf(past, kMinGain, kMaxGain);
  }

  float glide() {
    lead_ += kGlide * (target_ - lead_);
    gain_ += kGlide * (lead_ - gain_);
    return gain_;
  }

  float want_ = 0.0f;    // running power the crossfade's law intends
  float got_ = 0.0f;     // running power the crossfade gives
  float target_ = 1.0f;  // the gain the running powers ask for
  float lead_ = 1.0f;    // the first one-pole of the gain's glide
  float gain_ = 1.0f;    // the gain as applied
  int count_ = 0;        // samples since the target was last worked out
};

}  // namespace cv
