#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/harmony.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/shifters.h"
#include "engine/smooth.h"

// Autotune pulls the sung pitch to the nearest note of the key's major scale,
// or to the nearest semitone in chromatic mode. RESPONSE is the one-pole time
// constant on the correction. PitchFx owns the ring, the tracker, and the
// crossfade from the dry voice to the corrected one.

namespace cv {

struct AutotuneTuning {
  float trimDb = 0.0f;            // level rule
  float maxCorrectSemis = 6.0f;   // never pull more than this (tracker glitch guard)
  ShifterTuning shifter;          // grainPeriods / epochSearch / epochLpHz (B)
};

struct AutotuneParams {
  bool on = false;                // panel toggle
  int engine = 1;                 // 0 A grid PSOLA, 1 B epoch PSOLA
  bool chromatic = false;         // nearest semitone; key ignored
  int key = 0;                    // kKeyRoot index, as Harmony
  float responseMs = 100.0f;       // panel knob, 5..500
  AutotuneTuning tuning;
};

class AutotuneVoice {
 public:
  static constexpr float kMinResponseMs = 5.0f;
  static constexpr float kMaxResponseMs = 500.0f;

  AutotuneVoice() { reset(); }

  void reset() {
    psola_.reset();
    epoch_.reset();
    corr_ = 0.0f;
    target_ = 0.0f;
    a_ = 1.0f;
    gain_ = 1.0f;
    grainPeriods_ = ShifterTuning{}.grainPeriods;
    search_ = ShifterTuning{}.epochSearch;
    engine_ = 1;
    fresh_ = true;
  }

  // Per block: sets the target correction from pr and returns the smoothed
  // correction in semitones, 0 when off. Unvoiced frames hold the target.
  // The first voiced frame after reset or after off snaps to the target.
  float prepare(const PitchResult& pr, const AutotuneParams& p) {
    const AutotuneTuning& t = p.tuning;
    const int engine = p.engine == 0 ? 0 : 1;
    if (engine != engine_) {
      if (engine == 0) epoch_.reset();
      else psola_.reset();
      engine_ = engine;
    }
    a_ = smooth::coef(clampf(p.responseMs, kMinResponseMs, kMaxResponseMs) * 0.001f);
    gain_ = powf(10.0f, t.trimDb / 20.0f);
    grainPeriods_ = t.shifter.grainPeriods;
    search_ = t.shifter.epochSearch;
    if (!p.on) {
      fresh_ = true;
      return 0.0f;
    }
    if (pr.voiced && pr.hz > 0.0f) {
      const float maxC = t.maxCorrectSemis > 0.0f ? t.maxCorrectSemis : 0.0f;
      target_ = clampf(correction(pr.hz, p.key, p.chromatic), -maxC, maxC);
      if (fresh_) corr_ = target_;
      fresh_ = false;
    }
    return corr_;
  }

  // Per sample: the corrected voice, trim applied. lp is the ring low-passed
  // for engine B's peak search. Before the first voiced frame it passes the
  // input at the shifter's latency.
  float tick(const VoiceRing& ring, const VoiceRing& lp, long writeCount, float period) {
    corr_ = smooth::step(corr_, target_, a_);
    if (period <= 0.0f) {
      const int head = static_cast<int>(writeCount % kVoiceRingLen);
      const int i = head - PsolaVoice::kGrainDelay;
      return gain_ * ring[i < 0 ? i + kVoiceRingLen : i];
    }
    const float ratio = exp2f(corr_ / 12.0f);
    if (engine_ == 0) return gain_ * psola_.tick(ring, writeCount, period, ratio);
    return gain_ * epoch_.tick(ring, lp, writeCount, period, ratio, 1.0f, grainPeriods_, search_);
  }

  // Smoothed correction in semitones.
  float correctionSemis() const { return corr_; }

 private:
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
  float grainPeriods_ = 2.0f;
  float search_ = 0.25f;
  int engine_ = 1;
  bool fresh_ = true;
};

}  // namespace cv
