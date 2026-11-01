#pragma once

#include <cmath>
#include <limits>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/scale.h"
#include "engine/smooth.h"

// Autotune pulls the sung pitch to the nearest note of the key's major scale
// with a grid PSOLA voice. RESPONSE is the one-pole time constant on the
// correction. PitchFx owns the ring, the tracker, and the crossfade from the
// dry voice to the corrected one.

namespace cv {

struct AutotuneTuning {
  float trimDb = 0.0f;            // level rule
  float maxCorrectSemis = 6.0f;   // never pull more than this (tracker glitch guard)
  float grainDelay = 1040.0f;     // output latency, samples
};

struct AutotuneParams {
  bool on = false;                // panel toggle
  int key = 0;                    // kKeyRoot index
  float responseMs = 1.0f;        // panel knob, 1..500; 1 = hard tune
  AutotuneTuning tuning;
};

class AutotuneVoice {
 public:
  static constexpr float kMinResponseMs = 1.0f;
  static constexpr float kMaxResponseMs = 500.0f;

  // The RESPONSE knob runs NATURAL (0, slow glide) to MECHANICAL (1, hard tune), log taper.
  static float responseMsAt(float mechanical) {
    return kMaxResponseMs * powf(kMinResponseMs / kMaxResponseMs, clampf(mechanical, 0.0f, 1.0f));
  }
  static float mechanicalOf(float responseMs) {
    const float ms = clampf(responseMs, kMinResponseMs, kMaxResponseMs);
    return logf(kMaxResponseMs / ms) / logf(kMaxResponseMs / kMinResponseMs);
  }

  AutotuneVoice() { reset(); }

  void reset() {
    psola_.reset();
    corr_ = 0.0f;
    target_ = 0.0f;
    fresh_ = true;
    responseMs_ = kUnset;
    trimDb_ = kUnset;
    degreeHz_ = kUnset;
  }

  // Per block: sets the target correction from pr and returns the smoothed correction in
  // semitones, 0 when off. Unvoiced frames hold the target. The first voiced frame after reset()
  // snaps to the target; after that the correction always glides. libm runs only when its input
  // moved: RESPONSE and the trim when they change, the scale degree when a hop publishes a new frame.
  float prepare(const PitchResult& pr, const AutotuneParams& p) {
    const AutotuneTuning& t = p.tuning;
    if (p.responseMs != responseMs_) {
      responseMs_ = p.responseMs;
      a_ = smooth::coef(clampf(p.responseMs, kMinResponseMs, kMaxResponseMs) * 0.001f);
    }
    if (t.trimDb != trimDb_) {
      trimDb_ = t.trimDb;
      gain_ = powf(10.0f, t.trimDb / 20.0f);
    }
    psola_.setGrainDelay(t.grainDelay);
    if (!p.on) return 0.0f;
    if (pr.voiced && pr.hz > 0.0f) {
      if (pr.rawHz != degreeHz_ || p.key != degreeKey_) {
        degreeHz_ = pr.rawHz;
        degreeKey_ = p.key;
        deviation_ = degreeOf(pr.rawHz, p.key).dev;
      }
      const float maxC = t.maxCorrectSemis > 0.0f ? t.maxCorrectSemis : 0.0f;
      target_ = clampf(-deviation_, -maxC, maxC);
      if (fresh_) corr_ = target_;
      fresh_ = false;
    }
    return corr_;
  }

  // Per sample: the corrected voice, trim applied. The voice takes the inverse pitch ratio, so it
  // multiplies where it would divide. PitchFx fades it in only once the tracker has a period, so the
  // grains start on the sung pitch.
  float tick(const VoiceRing& ring, long writeCount, float period) {
    corr_ = smooth::step(corr_, target_, a_);
    return gain_ * psola_.tick(ring, writeCount, period, exp2Fast(corr_ * (-1.0f / 12.0f)));
  }

  // Smoothed correction in semitones.
  float correctionSemis() const { return corr_; }

 private:
  // NaN equals no input, so a cleared input recomputes on the next prepare().
  static constexpr float kUnset = std::numeric_limits<float>::quiet_NaN();

  PsolaVoice psola_;
  float corr_ = 0.0f;
  float target_ = 0.0f;
  float a_ = 1.0f;
  float gain_ = 1.0f;
  bool fresh_ = true;
  float responseMs_ = kUnset;  // the inputs a_, gain_ and deviation_ were computed from
  float trimDb_ = kUnset;
  float degreeHz_ = kUnset;
  int degreeKey_ = 0;
  float deviation_ = 0.0f;  // the sung pitch's distance from its scale note, semitones
};

}  // namespace cv
