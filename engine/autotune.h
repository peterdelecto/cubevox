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

// Autotune pulls the sung pitch to a note of the key's major scale with a grid
// PSOLA voice: the nearest note, unless the last one still holds (scale.h).
// RESPONSE is the one-pole time constant on the correction. PitchFx owns the
// ring, the tracker, and the crossfade from the dry voice to the corrected one.

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
    responseMs_ = kUnset;
    trimDb_ = kUnset;
    degreeHz_ = kUnset;
    haveNote_ = false;
  }

  // Per block: sets the target correction from pr and returns the smoothed correction in
  // semitones, 0 when off. Unvoiced frames hold the target. A clamped frame reads the tracker's
  // edge, not the note, so its target is 0 and the note passes uncorrected. The correction starts
  // from 0 after reset() and always glides at RESPONSE. libm runs only when its input moved:
  // RESPONSE and the trim when they change, the scale note when a hop publishes a new frame.
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
    if (pr.voiced && pr.clamped) {
      target_ = 0.0f;
    } else if (pr.voiced && pr.hz > 0.0f) {
      const float hz = sungHz(pr);
      if (hz != degreeHz_ || p.key != degreeKey_) pullTo(hz, p.key);
      const float maxC = t.maxCorrectSemis > 0.0f ? t.maxCorrectSemis : 0.0f;
      target_ = clampf(-deviation_, -maxC, maxC);
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
  static constexpr float kFifthRatio = 1.49830708f;  // 2^(7/12)

  // The pitch the target follows. The newest frame is one hop fresher than the median, so it leads
  // while it sits within a fifth of the median; past that the median stands in, so a frame read at
  // two or three times the period never reaches the target. A sung leap stays inside the fifth and
  // lands on its own hop.
  static float sungHz(const PitchResult& pr) {
    const bool withinFifth = pr.rawHz < pr.hz * kFifthRatio && pr.rawHz * kFifthRatio > pr.hz;
    return withinFifth ? pr.rawHz : pr.hz;
  }

  // The scale note the pitch hz pulls to, and the pitch's distance from it. The first pick after
  // reset() or a KEY change takes the nearest note; every later pick holds the last note within
  // kHoldSemis past the halfway point (scale.h).
  void pullTo(float hz, int key) {
    const float midi = midiOf(hz);
    note_ = haveNote_ && key == degreeKey_ ? heldNote(midi, key, note_) : midi - degreeOfMidi(midi, key).dev;
    haveNote_ = true;
    degreeHz_ = hz;
    degreeKey_ = key;
    deviation_ = midi - note_;
  }

  PsolaVoice psola_;
  float corr_ = 0.0f;
  float target_ = 0.0f;
  float a_ = 1.0f;
  float gain_ = 1.0f;
  float responseMs_ = kUnset;  // the inputs a_, gain_ and deviation_ were computed from
  float trimDb_ = kUnset;
  float degreeHz_ = kUnset;
  int degreeKey_ = 0;
  float deviation_ = 0.0f;  // the sung pitch's distance from its scale note, semitones
  float note_ = 0.0f;       // the scale note the last pick pulled to, as a MIDI pitch
  bool haveNote_ = false;
};

}  // namespace cv
