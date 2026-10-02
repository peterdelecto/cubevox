#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/harmony.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/shifters.h"
#include "engine/smooth.h"

// Autotune: pulls the sung pitch to the nearest note of the key's major scale,
// or the nearest semitone in chromatic mode. RESPONSE is the one-pole time
// constant on the correction. PitchFx owns the ring, the tracker, and the
// crossfade between the dry voice and the corrected one.

namespace cv {

struct AutotuneTuning {
  float maxCorrectionSemis = 2.0f;   // never pull more than this
  float formant = 0.0f;              // engine B, semitones, 0 = same singer
  float trimDb = 0.2f;               // level rule
  int engine = 1;                    // 0 grid PSOLA, 1 epoch PSOLA
};

struct AutotuneParams {
  bool on = false;                   // panel toggle
  bool followHarmonyKey = true;      // the panel's one KEY encoder serves both
  int key = 0;                       // kKeyRoot index, used when !followHarmonyKey
  bool chromatic = false;            // nearest semitone; key ignored
  float responseMs = 40.0f;          // panel knob, 1..300
  AutotuneTuning tuning;
};

class Autotune {
 public:
  static constexpr float kMinResponseMs = 1.0f;
  static constexpr float kMaxResponseMs = 300.0f;
  static constexpr float kMaxFormantSemis = 12.0f;

  Autotune() { reset(); }

  void reset() {
    psola_.reset();
    epoch_.reset();
    corr_ = 0.0f;
    target_ = 0.0f;
    a_ = 1.0f;
    gain_ = 1.0f;
    formant_ = 1.0f;
    engine_ = 1;
    fresh_ = true;
  }

  // Per block: sets the target correction from the tracker result. Unvoiced
  // frames hold the target. The first voiced frame after reset or after the
  // stage was off snaps the correction to its target.
  void prepare(const PitchResult& pr, const AutotuneParams& p, int harmonyKey) {
    const AutotuneTuning& t = p.tuning;
    const int engine = t.engine == 0 ? 0 : 1;
    if (engine != engine_) {
      if (engine == 0) epoch_.reset();
      else psola_.reset();
      engine_ = engine;
    }
    if (!p.on) {
      fresh_ = true;
    } else if (pr.voiced && pr.hz > 0.0f) {
      const int key = p.followHarmonyKey ? harmonyKey : p.key;
      const float maxC = t.maxCorrectionSemis > 0.0f ? t.maxCorrectionSemis : 0.0f;
      target_ = clampf(correction(pr.hz, key, p.chromatic), -maxC, maxC);
      if (fresh_) corr_ = target_;
      fresh_ = false;
    }
    const float ms = clampf(p.responseMs, kMinResponseMs, kMaxResponseMs);
    a_ = smooth::coef(ms * 0.001f);
    gain_ = powf(10.0f, t.trimDb / 20.0f);
    formant_ = exp2f(clampf(t.formant, -kMaxFormantSemis, kMaxFormantSemis) / 12.0f);
  }

  // Smoothed correction in semitones, for Octave and Harmony to add.
  float correctionSemis() const { return corr_; }

  // Per sample: the corrected voice, wet only. Before the first voiced frame
  // it passes the input at the shifter's latency.
  float tick(const VoiceRing& ring, const VoiceRing& lpRing, long writeCount, float period) {
    corr_ = smooth::step(corr_, target_, a_);
    if (period <= 0.0f) {
      const int head = static_cast<int>(writeCount % kVoiceRingLen);
      const int i = head - PsolaVoice::kGrainDelay;
      return gain_ * ring[i < 0 ? i + kVoiceRingLen : i];
    }
    const float ratio = exp2f(corr_ / 12.0f);
    if (engine_ == 0) return gain_ * psola_.tick(ring, writeCount, period, ratio);
    return gain_ * epoch_.tick(ring, lpRing, writeCount, period, ratio, formant_,
                               kShifter.grainPeriods, kShifter.epochSearch);
  }

 private:
  static constexpr ShifterTuning kShifter{};

  static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

  // Semitones from the sung pitch to its target note; ties snap down.
  static float correction(float hz, int key, bool chromatic) {
    if (!chromatic) return -HarmonyVoices::degreeOf(hz, key).dev;
    const float midi = 69.0f + 12.0f * log2f(hz / 440.0f);
    return ceilf(midi - 0.5f) - midi;
  }

  PsolaVoice psola_;
  EpochPsolaVoice epoch_;
  float corr_ = 0.0f;
  float target_ = 0.0f;
  float a_ = 1.0f;
  float gain_ = 1.0f;
  float formant_ = 1.0f;
  int engine_ = 1;
  bool fresh_ = true;
};

}  // namespace cv
