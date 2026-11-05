#pragma once

#include <array>
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
// RESPONSE is the one-pole time constant on the correction. AutotuneFx owns the
// ring, the tracker, and the crossfade from the dry voice to the corrected one.

namespace cv {

// grainDelay lines the voice up with the tracker's frame for hard tune (psola.h). The voice's latency
// is the grain delay plus one period, 22.7 ms at 1 kHz to 36.0 ms at 70 Hz. trimDb is the level rule.
// Without it the voice reads -0.27 to +0.02 dB on the mean of Adam's three clips across the twelve keys
// and RESPONSE from hard tune to 500 ms, and the rule is 0 to +0.5 dB.
struct AutotuneTuning {
  float trimDb = 0.35f;
  float grainDelay = static_cast<float>(PsolaVoice::kGrainDelay);  // samples, tracker alignment
};

struct AutotuneParams {
  bool on = false;                // panel toggle
  int key = 0;                    // kKeyRoot index
  float responseMs = 1.0f;        // panel knob, 1..500; 1 = hard tune
  AutotuneTuning tuning;
};

// The ring length is a template argument, the Mob card's ring by default.
template <int RingLen = kVoiceRingLen>
class AutotuneVoiceT {
 public:
  using Ring = typename PsolaVoiceT<RingLen>::Ring;

  static constexpr float kMinResponseMs = 1.0f;
  static constexpr float kMaxResponseMs = 500.0f;

  // The RESPONSE knob runs NATURAL (0, slow glide) to MECHANICAL (1, hard tune), log taper.
  static float responseMsAt(float mechanical) {
    return logTaper(kMaxResponseMs, kMinResponseMs, clampf(mechanical, 0.0f, 1.0f));
  }
  static float mechanicalOf(float responseMs) {
    const float ms = clampf(responseMs, kMinResponseMs, kMaxResponseMs);
    return logf(kMaxResponseMs / ms) / logf(kMaxResponseMs / kMinResponseMs);
  }

  AutotuneVoiceT() { reset(); }

  void reset() {
    psola_.reset();
    corr_ = 0.0f;
    noteOffset_ = 0.0f;
    target_ = 0.0f;
    responseMs_ = kUnset;
    trim_ = DbGain{};
    degreeHz_ = kUnset;
    haveNote_ = false;
    restSamples_ = kRestSamples;
  }

  // Per call of n samples: sets the target correction from pr and returns the correction in
  // semitones, 0 when off. Unvoiced frames hold the target. A clamped frame reads the tracker's
  // edge, not the note, so its target is 0 and the note passes uncorrected. The correction starts
  // from 0 after reset() and follows the target at RESPONSE. A KEY change made while a note sounds
  // moves the note along kKeyGlideSec instead of at once, and the correction still follows the sung
  // pitch at RESPONSE through it, so hard tune holds. A change made in a rest takes effect at once.
  // libm runs only when its input moved: RESPONSE when it changes, the scale note when a hop
  // publishes a new frame.
  float prepare(const PitchResult& pr, const AutotuneParams& p, int n) {
    const AutotuneTuning& t = p.tuning;
    if (p.responseMs != responseMs_) setResponse(p.responseMs);
    trim_.set(t.trimDb);
    psola_.setTrim(trim_.gain());
    psola_.setGrainDelay(t.grainDelay);
    if (!p.on) return 0.0f;
    const bool afterRest = restSamples_ >= kRestSamples;
    restSamples_ = pr.voiced ? 0 : (restSamples_ + n < kRestSamples ? restSamples_ + n : kRestSamples);
    if (restSamples_ >= kRestSamples) foldNoteOffset();  // a rest ends the glide
    if (pr.voiced && pr.clamped) {
      target_ = 0.0f;
    } else if (pr.voiced) {
      const float hz = sungHz(pr);
      if (hz != degreeHz_ || p.key != degreeKey_) pullTo(hz, p.key, afterRest);
      target_ = clampf(-deviation_, -kMaxCorrectionSemis, kMaxCorrectionSemis);
    }
    return correctionSemis();
  }

  // The corrected voice for the n samples, at most kBlock, whose first sits at ring index `first`,
  // which the ring already holds: the correction steps sample by sample and sets each sample's
  // inverse pitch ratio, and the voice renders the call with it. The voice takes the inverse ratio,
  // so it multiplies where it would divide. AutotuneFx fades it in only once the tracker has a period,
  // so the grains start on the sung pitch.
  void render(const Ring& ring, int first, int n, float period, float* out) {
    stepCorrection<true>(n);
    psola_.render(ring, first, n, period, inv_.data(), out);
  }

  // n samples of the correction's glide without the voice, for a call whose corrected voice nobody
  // hears.
  void glide(int n) { stepCorrection<false>(n); }

  // Drops the grains in flight. The correction keeps its place.
  void restartVoice() { psola_.reset(); }

  // The correction in semitones: the part that follows the sung pitch plus the KEY glide's.
  float correctionSemis() const { return corr_ + noteOffset_; }

  // The correction after k more samples. Its first part closes on the target by (1 - a)^k and its
  // KEY glide runs down by its own decay. A reader that holds one ratio for a call of n samples
  // takes it at k = n / 2, the middle of the call, where the correction sits on average.
  float correctionAfter(float k) const {
    const float glide = noteOffset_ == 0.0f ? 0.0f : noteOffset_ * exp2Fast(k * offsetLog2_);
    return target_ + (corr_ - target_) * exp2Fast(k * logDecay_) + glide;
  }

  // How far the corrected voice lags the input at this period, in samples: the grain delay plus one
  // period. The Formant card reads it to put its voice on the same sample.
  float voiceDelay(float period) const { return psola_.latency(period); }

 private:
  // NaN equals no input, so a cleared input recomputes on the next prepare().
  static constexpr float kUnset = std::numeric_limits<float>::quiet_NaN();
  static constexpr float kFifthRatio = 1.49830708f;  // 2^(7/12)

  // The target holds the pitch's distance from its note, which stays under half the 2 st gap between
  // scale notes plus kHoldSemis. The correction follows it, and the KEY glide carries the correction
  // from the old note's side of that, so the sum stays near this bound and the polynomial below
  // (good to 2e-10 up to 1.25 st, 4e-8 at 3 st) needs no clamp.
  static constexpr float kMaxCorrectionSemis = 1.25f;

  // 2^(-corr / 12), the inverse of the pitch ratio, as the degree-5 Taylor polynomial of 2^x in
  // x = -corr / 12.
  static float inverseRatio(float corr) {
    const float x = corr * (-1.0f / 12.0f);
    return 1.0f + x * (0.6931472f + x * (0.2402265f + x * (0.05550411f + x * (0.009618129f + x * 0.001333356f))));
  }

  // A KEY change that moves the target note moves it by up to a semitone at once. At hard tune the
  // correction would cover 99 % of that in 4 ms, inside one grain (9 ms at 220 Hz, 28 ms at 70 Hz),
  // and the grains laid at the old and new spacing sum over the steady level. So the correction keeps
  // the old note's side of the change, noteOffset_, and lets it run down along this time constant,
  // while the part of the correction that follows the sung pitch moves to the new note at once. A
  // grain is two periods, so a low voice needs a long glide.
  static constexpr float kKeyGlideSec = 0.050f;

  // The glide's decay per sample as a power of two, and where it counts as done: the offset is under
  // this many semitones (a tenth of a cent).
  static inline const float kKeyGlideLog2 = log2f(smooth::keep(kKeyGlideSec, 1));
  static constexpr float kKeyGlideDoneSemis = 0.001f;

  // A KEY change found after this many samples without a voiced frame, 16 ms, has no note to protect:
  // the next note takes the new key at once.
  static constexpr int kRestSamples = 768;

  // RESPONSE sets the one-pole's coefficient. The KEY glide never runs faster than RESPONSE does, so
  // at a slow RESPONSE the correction moves to a new key at that pace.
  void setResponse(float responseMs) {
    responseMs_ = responseMs;
    const float sec = clampf(responseMs, kMinResponseMs, kMaxResponseMs) * 0.001f;
    a_ = smooth::coef(sec);
    logDecay_ = log2f(smooth::keep(sec, 1));
    offsetLog2_ = fmaxf(kKeyGlideLog2, logDecay_);
    offsetKeep_ = exp2Fast(offsetLog2_);
  }

  // The KEY glide's offset joins the part that follows the pitch; the sum does not move.
  void foldNoteOffset() {
    corr_ += noteOffset_;
    noteOffset_ = 0.0f;
  }

  // Steps the correction over n samples, and with Ratios sets each sample's inverse pitch ratio in
  // inv_. The last step snaps to the target once it is within smooth::kSnap, so a settled correction
  // holds its target bit for bit.
  template <bool Ratios>
  void stepCorrection(int n) {
    float c = corr_;
    float o = noteOffset_;
    for (int i = 0; i < n - 1; ++i) {
      c += a_ * (target_ - c);
      o *= offsetKeep_;
      if (Ratios) inv_[static_cast<size_t>(i)] = inverseRatio(c + o);
    }
    if (n > 0) {
      c = smooth::step(c, target_, a_);
      o *= offsetKeep_;
      if (Ratios) inv_[static_cast<size_t>(n - 1)] = inverseRatio(c + o);
    }
    corr_ = c;
    noteOffset_ = o;
    if (fabsf(o) < kKeyGlideDoneSemis) foldNoteOffset();
  }

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
  // kHoldSemis past the halfway point (scale.h). A KEY change while a note sounds starts the glide:
  // the part of the correction that follows the pitch moves to the new note, and the offset keeps the
  // old note's side. afterRest is true for the first frame after a rest, which has no note to protect.
  void pullTo(float hz, int key, bool afterRest) {
    const float midi = midiOf(hz);
    const float last = note_;
    const bool changed = haveNote_ && key != degreeKey_;
    note_ = haveNote_ && !changed ? heldNote(midi, key, note_) : midi - scaleDeviation(midi, key);
    if (changed && !afterRest) {
      corr_ += note_ - last;
      noteOffset_ += last - note_;
    }
    haveNote_ = true;
    degreeHz_ = hz;
    degreeKey_ = key;
    deviation_ = midi - note_;
  }

  PsolaVoiceT<RingLen> psola_;
  std::array<float, kBlock> inv_{};  // the inverse pitch ratio of each sample of a render
  float corr_ = 0.0f;                // the part of the correction that follows the sung pitch
  float noteOffset_ = 0.0f;          // the KEY glide's part: the old note's side of the change, running down
  float target_ = 0.0f;
  float a_ = 1.0f;
  float logDecay_ = -std::numeric_limits<float>::infinity();  // log2(1 - a_)
  float offsetLog2_ = kKeyGlideLog2;                          // the offset's decay per sample, a power of two
  float offsetKeep_ = 1.0f;                                   // the same as a factor
  DbGain trim_;
  float responseMs_ = kUnset;  // the input a_ was computed from
  float degreeHz_ = kUnset;
  int degreeKey_ = 0;
  float deviation_ = 0.0f;  // the sung pitch's distance from its scale note, semitones
  float note_ = 0.0f;       // the scale note the last pick pulled to, as a MIDI pitch
  int restSamples_ = kRestSamples;  // samples since the last voiced frame, up to kRestSamples
  bool haveNote_ = false;
};

using AutotuneVoice = AutotuneVoiceT<>;

}  // namespace cv
