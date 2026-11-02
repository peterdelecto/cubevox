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

// grainDelay lines the voice up with the tracker's frame for hard tune (psola.h). The voice's latency
// is the grain delay plus one period, 22.7 ms at 1 kHz to 36.0 ms at 70 Hz.
struct AutotuneTuning {
  float trimDb = 0.0f;  // level rule
  float grainDelay = static_cast<float>(PsolaVoice::kGrainDelay);  // samples, tracker alignment
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
    keyGlide_ = false;
  }

  // Per block: sets the target correction from pr and returns the smoothed correction in
  // semitones, 0 when off. Unvoiced frames hold the target. A clamped frame reads the tracker's
  // edge, not the note, so its target is 0 and the note passes uncorrected. The correction starts
  // from 0 after reset() and glides at RESPONSE, or at the 20 ms knob smoother when that is slower
  // and a KEY change moved the target. libm runs only when its input moved: RESPONSE and the trim
  // when they change, the scale note when a hop publishes a new frame.
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
    if (!p.on) {
      setStepCoef(a_);
      return 0.0f;
    }
    if (keyGlide_ && fabsf(corr_ - target_) < kKeyGlideDoneSemis) keyGlide_ = false;
    if (pr.voiced && pr.clamped) {
      target_ = 0.0f;
    } else if (pr.voiced) {
      const float hz = sungHz(pr);
      if (hz != degreeHz_ || p.key != degreeKey_) pullTo(hz, p.key);
      target_ = -deviation_;
    }
    setStepCoef(keyGlide_ && smooth::kSmoothCoef < a_ ? smooth::kSmoothCoef : a_);
    return corr_;
  }

  // Per sample: the corrected voice, trim applied. The voice takes the inverse pitch ratio, so it
  // multiplies where it would divide. PitchFx fades it in only once the tracker has a period, so the
  // grains start on the sung pitch.
  CV_INLINE float tick(const VoiceRing& ring, int head, float period) {
    corr_ = smooth::step(corr_, target_, stepCoef_);
    return gain_ * psola_.tick(ring, head, period, exp2Fast(corr_ * (-1.0f / 12.0f)));
  }

  // One sample of the correction's glide without the voice, for a sample whose corrected voice
  // nobody hears.
  void glide() { corr_ = smooth::step(corr_, target_, stepCoef_); }

  // Drops the grains in flight. The correction keeps its place.
  void restartVoice() { psola_.reset(); }

  // The voice's grid, and taking another voice's, so the two launch on the same samples.
  VoiceGrid grid() const { return psola_.grid(); }
  void adoptGrid(const VoiceGrid& g) { psola_.adoptGrid(g); }

  // Smoothed correction in semitones.
  float correctionSemis() const { return corr_; }

  // The correction after k more samples of its one-pole, which closes on the target by (1 - a)^k.
  // A reader that holds one ratio for a call of n samples takes it at k = n / 2, the middle of the
  // call, where the correction sits on average. logDecay_ follows stepCoef_ in prepare().
  float correctionAfter(float k) const {
    return target_ + (corr_ - target_) * exp2Fast(k * logDecay_);
  }

  // How far the corrected voice lags the input at this period, in samples: the grain delay plus one
  // period. The Formant card reads it to put its voice on the same sample.
  float voiceDelay(float period) const { return psola_.latency(period); }

 private:
  // NaN equals no input, so a cleared input recomputes on the next prepare().
  static constexpr float kUnset = std::numeric_limits<float>::quiet_NaN();
  static constexpr float kFifthRatio = 1.49830708f;  // 2^(7/12)

  // libm runs only when the coefficient moved.
  void setStepCoef(float c) {
    if (c == stepCoef_) return;
    stepCoef_ = c;
    logDecay_ = log2f(1.0f - c);
  }

  // A KEY change that moves the target note moves it by up to a semitone at once. At hard tune the
  // correction would cover 99 % of that in 4 ms, inside one grain (9 ms at 220 Hz), and the grains
  // laid at the old and new spacing sum 6 % over the steady level. The 20 ms glide keeps the step
  // under x1.02. The glide ends once the correction sits this close to the target, 2 cents.
  static constexpr float kKeyGlideDoneSemis = 0.02f;

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
    if (haveNote_ && key != degreeKey_) keyGlide_ = true;
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
  float stepCoef_ = 1.0f;  // a_, or the 20 ms knob smoother's coefficient while a KEY change glides
  float logDecay_ = -std::numeric_limits<float>::infinity();  // log2(1 - stepCoef_)
  float gain_ = 1.0f;
  float responseMs_ = kUnset;  // the inputs a_, gain_ and deviation_ were computed from
  float trimDb_ = kUnset;
  float degreeHz_ = kUnset;
  int degreeKey_ = 0;
  float deviation_ = 0.0f;  // the sung pitch's distance from its scale note, semitones
  float note_ = 0.0f;       // the scale note the last pick pulled to, as a MIDI pitch
  bool haveNote_ = false;
  bool keyGlide_ = false;  // a KEY change moved the target and the correction has not reached it
};

}  // namespace cv
