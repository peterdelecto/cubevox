#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/pitch.h"
#include "engine/pitch_fx.h"
#include "engine/scale.h"
#include "engine/smooth.h"

namespace cv {

// The Ring mod card. A sine carrier at RATIO times a held note multiplies the voice, so the clang stays in
// tune and follows the melody in steps. The note is the one nearest the tracked pitch. It is a major-scale
// note of the Autotune card's key when that card is placed, and any semitone (A4 = 440 Hz) when not. The
// held note changes only when the tracked pitch sits closer to another note by more than 0.3 semitone, so
// vibrato leaves the carrier still, and only when that same note has won for 60 ms in a row, so a stray
// tracker frame at an onset leaves it still too. The count restarts when the candidate changes or the margin
// test fails, and unvoiced frames pause it without touching the held note. The first voiced note after a
// reset sets the carrier at once. A settled note moves the carrier in the same block, with no glide, and the
// phase counter carries on. The carrier is a phase counter read through sinTurns. Unvoiced frames hold the
// last note, and before the first voiced frame the carrier sits at 220 Hz times RATIO.
// MIX is an equal-power crossfade, and the wet alone is trimmed to unity.

constexpr int kRingModRatioCount = 7;
constexpr std::array<float, kRingModRatioCount> kRingModRatio = {0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f};
constexpr std::array<const char*, kRingModRatioCount> kRingModRatioName = {"X0.5", "X1",   "X1.5", "X2",
                                                                            "X2.5", "X3",   "X4"};
constexpr uint8_t kRingModDefaultRatioStep = 2;

struct RingModTuning {
  float holdSemis = 0.3f;   // the pitch must sit this much closer to another note before the held note changes
  float settleMs = 60.0f;   // a new note must win this long in a row before the carrier moves to it
  float restHz = 220.0f;    // the pitch the carrier assumes before the first voiced frame
  float trimDb = 3.15f;     // the wet path's gain; a sine carrier alone leaves the voice 3.01 dB down
};

struct RingModParams {
  bool on = false;
  float ratio = 1.5f;  // knob A (RATIO), the carrier's frequency over the pitch
  float mix = 0.5f;    // knob B (MIX), 0..1
  bool keyed = false;  // an Autotune card is placed, so the notes are its key's major scale
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
    hasNote_ = false;
    noteMidi_ = 0.0f;
    noteHz_ = 0.0f;
    pendingMidi_ = 0.0f;
    pendingSamples_ = 0;
    carrierHz_ = 0.0f;
    mix_ = 0.0f;
    wet_ = 0.0f;
    fresh_ = true;
  }

  // `pitch` has run this block. `key` is the Autotune KEY index whose major scale holds the notes, or a
  // negative number for every semitone. Off and settled, out equals in bit for bit. in and out may be the
  // same buffer.
  void process(const float* in, float* out, int n, const RingModParams& p, const PitchFx& pitch, int key) {
    if (n <= 0) return;
    trackNote(pitch.pitch(), key, p.tuning, n);
    const float wetTarget = p.on ? 1.0f : 0.0f;
    if (wet_ == 0.0f && wetTarget == 0.0f) {
      fresh_ = true;
      if (out != in) std::memmove(out, in, sizeof(float) * static_cast<size_t>(n));
      return;
    }

    // The carrier sits on RATIO times the held note for the whole block.
    const float baseHz = hasNote_ ? noteHz_ : p.tuning.restHz;
    carrierHz_ = clampf(p.ratio * baseHz, 1.0f, ringmod_detail::kMaxCarrierHz);
    const uint32_t step = stepOf(carrierHz_);

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
    for (int i = 0; i < n; ++i) {
      phase += step;
      dry += dDry;
      wet += dWet;
      const float x = in[i];
      out[i] = x * (dry + wet * sinTurns(phase));
    }
    phase_ = phase;
  }

  // Test hook: the carrier's frequency in the last block, in Hz.
  float carrierHz() const { return carrierHz_; }

  // Test hook: the held note's frequency in Hz, or 0 before the first voiced frame.
  float heldHz() const { return hasNote_ ? noteHz_ : 0.0f; }

  // Runs once a block. Moves the held note to the one nearest a voiced pitch once the pitch has sat more than `hold` semitones
  // closer to it than to the held note for `settleMs` in a row. `n` is the block's length in samples. The
  // first voiced note, and a held note that is not in the scale (the key changed), take effect at once.
  // Unvoiced frames pause the count and leave the note alone.
  void trackNote(const PitchResult& pr, int key, const RingModTuning& t, int n) {
    if (!pr.voiced || pr.period <= 0.0f) return;
    const float midi = midiOf(kSampleRate / PitchTracker::clampPeriod(pr.period));
    const bool keyed = key >= 0;
    const int k = keyed ? key % kKeyCount : 0;
    const float nearest = keyed ? roundf(midi - scaleDeviation(midi, k)) : roundf(midi);
    const bool inScale = !keyed || fabsf(scaleDeviation(noteMidi_, k)) < 1e-3f;
    if (hasNote_ && inScale) {
      if (fabsf(midi - noteMidi_) - fabsf(midi - nearest) <= t.holdSemis) {
        pendingSamples_ = 0;
        return;
      }
      if (pendingSamples_ == 0 || nearest != pendingMidi_) {
        pendingMidi_ = nearest;
        pendingSamples_ = 0;
      }
      pendingSamples_ += n;
      if (static_cast<float>(pendingSamples_) < t.settleMs * kSmpPerMs) return;
    }
    hasNote_ = true;
    noteMidi_ = nearest;
    noteHz_ = 440.0f * exp2f((nearest - 69.0f) / 12.0f);
    pendingSamples_ = 0;
  }

 private:
  static uint32_t stepOf(float hz) { return static_cast<uint32_t>(hz * (4294967296.0f / kSampleRate)); }

  static float dryGain(float e) { return sinTurns(ringmod_detail::kQuarterTurn - phaseOf(e)); }
  static float wetGain(float e) { return sinTurns(phaseOf(e)); }
  static uint32_t phaseOf(float e) {
    return static_cast<uint32_t>(smooth::clamp01(e) * static_cast<float>(ringmod_detail::kQuarterTurn));
  }

  uint32_t phase_ = 0;
  bool hasNote_ = false;    // a voiced frame has set the note
  float noteMidi_ = 0.0f;   // the held note, a whole MIDI number
  float noteHz_ = 0.0f;
  float pendingMidi_ = 0.0f;  // the candidate note that is winning
  int pendingSamples_ = 0;    // how long it has won in a row, in samples; 0 when no candidate is winning
  float carrierHz_ = 0.0f;  // RATIO times the note, in the last block
  float mix_ = 0.0f;
  float wet_ = 0.0f;        // the dry-to-card fade, 0 dry .. 1 card
  bool fresh_ = true;       // the next call takes the knobs outright
};

}  // namespace cv
