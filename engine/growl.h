#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "engine/common.h"
#include "engine/distortion.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/formant_voice.h"
#include "engine/pitch.h"
#include "engine/pitch_fx.h"
#include "engine/psola.h"
#include "engine/smooth.h"
#include "engine/syllable_sweep.h"

// The Growl card, the gross monster voice of SOPHIE's "Whole New World". Three parts run in a row, and
// an equal-power MIX crossfades the result against the card's input. At MIX 100 % the dry signal is gone.
//
// The panel has two pots. The upper pot is GROWL. It sets the engine's rasp `growl` to kGrowlRaspTop times
// the pot and its fold `drive` to kGrowlDriveTop times the pot, a straight line each. The lower pot is MIX.
//
// 1. The drop. A grain voice cut from the pitch stage's input ring on the tracker's period grid plays
//    the voice at a pitch ratio of 2^(pitchSt / 12) and a grain playback rate of 2^(throatSt / 12),
//    an octave down with the throat 5 st down at rest. It follows Autotune's correction and reads at
//    Autotune's lag, where the tracker's frame lines up with the audio.
//
//    The throat moves with each syllable. The syllable sweep (`syllable_sweep.h`, Yawn's law) reads the
//    card's input, and throatSt is formantSt plus the sweep times throatDropTop times GROWL. At GROWL 100 %
//    a syllable drops the throat from 5 st down to 12 st down, and it returns over DECAY while the note
//    holds. At GROWL 0 the throat stays at formantSt. The throat reaches the voice through the 20 ms glide
//    GROWL and DRIVE use.
// 2. The rasp. A raised wave at half the tracked pitch multiplies the dropped voice, so alternate
//    periods of the sung note come out loud and quiet, the period doubling of a growl's false folds.
//    Over the top half of GROWL a wave at a third of the pitch blends in. The wave is a raised sine at
//    GROWL 0 and hardens toward a square as GROWL rises. A level gain holds the modulated voice at the
//    power of the plain one. Without a pitch (unvoiced, silence) the wave holds its last rate and its
//    depth falls to 0 over 20 ms, so breath does not buzz.
//
//    Each period of the half-pitch wave draws its own depth, the set depth times 1 - r * jitter, with r
//    uniform in 0..1 and jitter rising in a straight line from 0 at GROWL 0 to jitterTop at GROWL 100 %.
//    A period runs from one crest of the wave to the next. The wave is at gain 1 on a crest whatever the
//    depth, so the new depth steps nothing. The draws come from a generator seeded at reset, so a render
//    repeats bit for bit.
// 3. The fold. A low shelf ahead of Distortion's quadratic knee tilts the tone to the low mids, and a
//    peak after it adds bite. DRIVE sets the knee's input gain and the low shelf's depth. The peak
//    sits at peakHz with width peakQ, and its gain rises in a straight line from 0 dB at GROWL 0 to
//    peakDb at GROWL 100 %. A DC bias of biasTop times GROWL squared (in rail units) enters the knee with
//    the signal, so the knee clips the two sides unevenly and adds even harmonics. The fold's DC blocker
//    removes the offset after the knee. A makeup gain follows the knee, and snarlTrimDb times GROWL
//    trims it so the extra bite and bias leave the level where it was. At DRIVE 0 the fold is out and
//    the rasped voice passes clean. The knee runs at the base rate, with no oversampling.
//
//    A 2nd-order Butterworth high-pass at hpHz ends the wet path, ahead of MIX, so the dropped voice
//    leaves no rumble under the note.
//
//    The throat moves the grains' playback rate, and a grain keeps the rate it launched with. A moving rate
//    would still shift each new grain's pulse against the grid and bend the pitch, so the voice anchors
//    each grain's loudest pulse where the rest throat would put it (`FormantPsolaVoice::anchorPulse`).
//
// The ring holds the input of the slot where the pitch stage runs, so the stages between that slot and
// this card are not in the growl voice.
//
// The dry side of MIX reads that same ring at the lag the voice reads at, so the two line up and the
// blend does not flam. The lag follows the tracker's period. It moves at most kDryLagSlew samples per
// sample, so a new period slides the dry's delay and never jumps it. The dry costs a ring read per sample
// only while MIX sits under 100 %.

namespace cv {

// The pot-to-engine map: the GROWL pot at 100 % sets the rasp to this, and the fold's drive to the other.
constexpr float kGrowlRaspTop = 0.80f;
constexpr float kGrowlDriveTop = 0.65f;
constexpr float kGrowlDefaultPot = 0.75f;

struct GrowlTuning {
  float pitchSt = -12.0f;       // the drop; the voice's pitch ratio is 2^(pitchSt / 12)
  float formantSt = -5.0f;      // the throat at rest; the grains play at 2^(throatSt / 12)
  float throatDropTop = -7.0f;  // the sweep's added throat shift at GROWL 100 %
  float sensitivity = kSweepDefaultSensitivity;  // the syllable sweep's SENSITIVITY, 0..1
  float decayMs = kSweepDefaultDecayMs;          // a sweep falls from the drive to 0 over this
  float hardTop = 7.0f;         // the wave's added slope at GROWL 100 %, 1 + hardTop * GROWL^2 times a raised sine's
  float thirdTop = 0.5f;        // the third-of-pitch wave's share at GROWL 100 %
  float hpHz = 100.0f;          // the wet path's 2nd-order Butterworth high-pass, after the fold and ahead of MIX
  float lowShelfHz = 200.0f;    // ahead of the knee
  float lowShelfDb = 6.0f;      // at DRIVE 100 %
  float peakHz = 2500.0f;       // the bite peak, after the knee
  float peakQ = 1.8f;           // its width
  float peakDb = 10.0f;         // its gain at GROWL 100 % (the engine's DRIVE at kGrowlDriveTop)
  float biasTop = 0.25f;        // the DC added ahead of the knee at GROWL 100 %, in rail units (the rail's limit is 1)
  float snarlTrimDb = -0.45f;   // the fold's added trim at GROWL 100 %, in dB along the pot; holds the level against the bite and the bias
  float jitterTop = 0.55f;     // the most a period's depth is cut by, at GROWL 100 %
  float driveDb = 30.0f;        // the knee's input gain at DRIVE 100 %
  float makeupSat = 10.0f;      // the knee's input gain where the makeup law flattens
  float railSoft = 0.1f;        // the knee's soft edge as a fraction of the rail
  float railAsym = 0.05f;       // the negative rail sits at -(1 - railAsym)
  float trimDb = 2.40f;         // level rule: Adam's three clips at GROWL 75 % and MIX 100 % land 0 to +0.5 dB
};

struct GrowlParams {
  bool on = false;
  float growl = kGrowlDefaultPot * kGrowlRaspTop;    // the rasp, 0..1 (the GROWL pot times kGrowlRaspTop)
  float drive = kGrowlDefaultPot * kGrowlDriveTop;   // the fold, 0..1 (the GROWL pot times kGrowlDriveTop)
  float mix = 1.0f;                                  // knob B (MIX), 0..1
  GrowlTuning tuning;
};

namespace growl_detail {

// The ranges the engine clamps the tuning to. The render tool refuses a value outside them.
constexpr float kMaxSt = 12.0f;
constexpr float kMaxThroatDropSt = 24.0f;
constexpr float kMaxHardTop = 32.0f;
constexpr float kMaxDriveDb = 48.0f;
constexpr float kMinMakeupSat = 1.0f;
constexpr float kMaxMakeupSat = 1000.0f;
constexpr float kMaxShelfDb = 24.0f;
constexpr float kMaxPeakQ = 10.0f;
constexpr float kMaxBiasTop = 1.0f;
constexpr float kMaxTrimDb = 24.0f;
constexpr float kMinRailSoft = 0.01f;
constexpr float kMaxRailAsym = 0.5f;

// The depth's fall without a pitch and the dry-to-voice crossfade both take 20 ms, in straight lines.
constexpr float kFadeMs = 20.0f;
constexpr float kFadeStep = 1.0f / (kFadeMs * kSmpPerMs);

// The most the dry's delay moves per sample, 0.5 % of a sample, which bends the dry's pitch by 9 cents.
constexpr float kDryLagSlew = 0.005f;

// GROWL where the third-of-pitch wave starts to blend in.
constexpr float kThirdFrom = 0.5f;

// One turn of the wave's phase counter over six. The counter runs at a sixth of the tracked pitch, so
// three times it is the half-pitch wave and twice it the third-pitch wave, locked together.
constexpr float kSixthTurn = 4294967296.0f / 6.0f;

// The half-pitch wave's crest, a quarter turn in, and the jitter generator's seed.
constexpr uint32_t kCrestTurn = 0x40000000u;
constexpr uint32_t kJitterSeed = 0x9E3779B9u;

// E[c^2] for c = clamp(k sin x, -1, 1) over a turn: k^2 / 2 up to k = 1, then the clipped share.
// libm, so it runs per block at most.
inline float clippedSinePower(float k) {
  if (k <= 1.0f) return 0.5f * k * k;
  const float s0 = 1.0f / k;
  const float c0 = sqrtf(1.0f - s0 * s0);
  const float x0 = asinf(s0);
  return (2.0f / kPi) * (0.5f * k * k * (x0 - s0 * c0) + kHalfPi - x0);
}

// The fold's tilt, gain and makeup at one DRIVE.
struct FoldGains {
  float lowShelf = 0.0f;  // the low shelf's gain less 1
  float pre = 1.0f;       // the knee's input gain
  float peak = 0.0f;      // the bite peak's gain less 1
  float post = 1.0f;      // the makeup
  float bias = 0.0f;      // the DC added ahead of the knee
};

// The rasp's wave and gains at one GROWL and depth fade: m = clamp(0.5 + half * sin, 0, 1) for each
// wave, the third's share of the blend, and y = voice * (base + swing * (1 - u * (1 - m))), where u is
// the period's depth factor 1 - r * jitter.
struct RaspGains {
  float half = 0.5f;
  float third = 0.0f;
  float base = 1.0f;
  float swing = 0.0f;
  float jitter = 0.0f;
};

// The fold: a first-order low shelf, the knee's input gain, Distortion's rail, the bite peak, the
// makeup and a DC blocker for the rail's asymmetry. The peak adds g times a band-pass of fixed width
// and 0 dB gain at its centre to the signal, so its gain is 1 + g there and it glides without
// moving a pole. Its gains glide across a call from the last call's to this one's.
class Fold {
 public:
  void setTuning(const GrowlTuning& t) {
    lowLp_.set(t.lowShelfHz);
    setBand(t.peakHz, t.peakQ);
    const float soft = clampf(t.railSoft, kMinRailSoft, 1.0f);
    rail_ = Distortion::makeRail(1.0f - clampf(t.railAsym, 0.0f, kMaxRailAsym), soft);
    lowDb_ = clampf(t.lowShelfDb, -kMaxShelfDb, kMaxShelfDb);
    peakDb_ = clampf(t.peakDb, -kMaxShelfDb, kMaxShelfDb);
    driveDb_ = clampf(t.driveDb, 0.0f, kMaxDriveDb);
    sat_ = clampf(t.makeupSat, kMinMakeupSat, kMaxMakeupSat);
    biasTop_ = clampf(t.biasTop, 0.0f, kMaxBiasTop);
    snarlTrimDb_ = clampf(t.snarlTrimDb, -kMaxTrimDb, kMaxTrimDb);
    unityMakeup_ = makeup(1.0f);
    at_ = -1.0f;
  }

  // The gains at DRIVE d, worked out again only when d moves.
  const FoldGains& gainsAt(float d) {
    if (d != at_) {
      at_ = d;
      gains_.lowShelf = dbToGainFast(lowDb_ * d) - 1.0f;
      gains_.pre = dbToGainFast(driveDb_ * d);
      gains_.peak = dbToGainFast(clampf(peakDb_ * d * potPerDrive_, -kMaxShelfDb, kMaxShelfDb)) - 1.0f;
      const float pot = smooth::clamp01(d * potPerDrive_);
      gains_.post = unityMakeup_ / makeup(gains_.pre) * dbToGainFast(snarlTrimDb_ * pot);
      gains_.bias = biasTop_ * pot * pot;
    }
    return gains_;
  }

  // x through the fold in place over m samples, the gains running from g by step per sample.
  void run(float* x, int m, FoldGains g, const FoldGains& step) {
    const Distortion::Rail r = rail_;
    float lo = lowLp_.y;
    float dc = dc_.y;
    const float aLo = lowLp_.a;
    const float aDc = dc_.a;
    Biquad band = band_;
    for (int i = 0; i < m; ++i) {
      g.lowShelf += step.lowShelf;
      g.pre += step.pre;
      g.peak += step.peak;
      g.post += step.post;
      g.bias += step.bias;
      const float v = x[i];
      lo += aLo * (v - lo);
      const float c = Distortion::rail(r, (v + g.lowShelf * lo) * g.pre + g.bias);
      const float y = (c + g.peak * band.run(c)) * g.post;
      dc += aDc * (y - dc);
      x[i] = y - dc;
    }
    lowLp_.y = lo;
    band_ = band;
    dc_.y = dc;
  }

  void clear() {
    lowLp_.clear();
    band_.clear();
    dc_.clear();
  }

  // False once a NaN or an infinity got into a filter, which no later sample would wash out.
  bool finite() const { return std::isfinite(lowLp_.y + band_.z1 + band_.z2 + dc_.y); }

 private:
  // The band-pass at hz and q with 0 dB gain at its centre (RBJ constant-peak form).
  void setBand(float hz, float q) {
    const RbjCorner k = rbjCorner(hz);
    const float alpha = rbjAlpha(k.sw, clampf(q, kPeakMinQ, kMaxPeakQ));
    const float inv = 1.0f / (1.0f + alpha);
    band_.b0 = alpha * inv;
    band_.b1 = 0.0f;
    band_.b2 = -alpha * inv;
    band_.a1 = -2.0f * k.cw * inv;
    band_.a2 = (1.0f - alpha) * inv;
  }

  // The knee's output level against its input gain: linear at first, flat once the gain passes sat_.
  float makeup(float gain) const {
    const float r = gain / sat_;
    return gain / sqrtf(1.0f + r * r);
  }

  OnePole lowLp_;
  Biquad band_;
  DcBlocker dc_;
  Distortion::Rail rail_ = Distortion::makeRail(0.95f, 0.1f);
  FoldGains gains_;
  float lowDb_ = 0.0f;
  float peakDb_ = 0.0f;
  float biasTop_ = 0.0f;
  float snarlTrimDb_ = 0.0f;
  float potPerDrive_ = 1.0f / kGrowlDriveTop;  // the GROWL pot as a share of DRIVE: 1 where DRIVE is kGrowlDriveTop
  float driveDb_ = 0.0f;
  float sat_ = 10.0f;
  float unityMakeup_ = 1.0f;
  float at_ = -1.0f;  // the DRIVE gains_ was worked out at
};

}  // namespace growl_detail

class Growl {
 public:
  Growl() {
    voice_.latchGain(true);
    reset();
  }

  void reset() {
    voice_.reset();
    fold_.clear();
    hp_.clear();
    sweep_.reset();
    buf_.fill(0.0f);
    throat_ = 0.0f;
    throatTarget_ = 0.0f;
    heldPeriod_ = 0.0f;
    phase_ = 0;
    phaseStep_ = 0;
    rng_ = growl_detail::kJitterSeed;
    periodDepth_ = 1.0f;
    wet_ = 0.0f;
    depthFade_ = 0.0f;
    growl_ = 0.0f;
    drive_ = 0.0f;
    mix_ = 1.0f;
    dryLag_ = 0.0f;
    rasp_ = growl_detail::RaspGains{};
    raspAt_ = -1.0f;
    raspFadeAt_ = -1.0f;
    fresh_ = true;
    idle_ = true;
  }

  // in is this slot's input. pitch has already run this block, so the block's samples sit at ring
  // index head() - n + i. Off and settled, out equals in bit for bit. in and out may be the same
  // buffer.
  void process(const float* in, float* out, int n, const GrowlParams& p, const PitchFx& pitch) {
    const PitchResult& pr = pitch.pitch();
    if (pr.period > 0.0f) heldPeriod_ = pr.period;
    const float wetTarget = p.on && heldPeriod_ > 0.0f ? 1.0f : 0.0f;
    if (wet_ == 0.0f && wetTarget == 0.0f) {
      passThrough(in, out, n);
      return;
    }
    idle_ = false;
    applyTuning(p.tuning);
    const Call c = beginCall(p, pitch, in, n, wetTarget);
    int first = pitch.head() - n;
    if (first < 0) first += kVoiceRingLen;
    for (int at = 0; at < n; at += kBlock) {
      const int m = n - at < kBlock ? n - at : kBlock;
      buf_.fill(0.0f);
      voice_.render(pitch.ring(), wrapOnce<kVoiceRingLen>(first + at), m, buf_.data());
      raspChunk(c, at, m);
      if (c.fold) fold_.run(buf_.data(), m, foldAt(c, at), c.foldStep);
      highPass(m);
      if (c.dry) blendDry(c, pitch.ring(), wrapOnce<kVoiceRingLen>(first + at), m, at);
      mixChunk(c, in + at, out + at, m, at);
    }
    if (!fold_.finite()) fold_.clear();
    if (!std::isfinite(hp_.z1 + hp_.z2)) hp_.clear();
  }

  // Test hooks: the wave's depth fade, 1 with a pitch and 0 without, and the dry-to-voice crossfade.
  float depthFade() const { return depthFade_; }
  float wet() const { return wet_; }

  // Test hooks: the throat the sweep asks for and the throat the voice plays after the glide, in st.
  float throatTargetSt() const { return throatTarget_; }
  float throatSt() const { return throat_; }

  // Test hook: the delay the dry side reads at, in samples.
  float dryLagSamples() const { return dryLag_; }

  // Test hooks: the voice's pitch ratio, the rasp wave's rate in Hz, the jitter at the last call's end and
  // the running period's depth factor.
  float pitchRatio() const { return pitchRatio_; }
  float raspRateHz() const {
    return static_cast<float>(static_cast<double>(phaseStep_) * (3.0 * kSampleRate / 4294967296.0));
  }
  float jitter() const { return rasp_.jitter; }
  float periodDepth() const { return periodDepth_; }

  // Test hook: the syllable sweep, 0 to 1.
  float sweep() const { return sweep_.sweep(); }

 private:
  using RaspGains = growl_detail::RaspGains;
  using FoldGains = growl_detail::FoldGains;

  // What one call runs with: each gain at its start and its step per sample, so sample i of the call
  // takes start + (i + 1) * step.
  struct Call {
    RaspGains rasp0;
    RaspGains raspStep;
    FoldGains fold0;
    FoldGains foldStep;
    float wet0 = 0.0f;
    float wetStep = 0.0f;
    float voiceGain0 = 1.0f;  // MIX's equal-power pair: the voice's share and the dry's
    float voiceGainStep = 0.0f;
    float dryGain0 = 0.0f;
    float dryGainStep = 0.0f;
    float lag0 = 0.0f;  // the dry's delay in samples, and its step per sample
    float lagStep = 0.0f;
    bool dry = false;    // MIX sits under 100 % somewhere in the call
    bool rasp = false;   // the wave moves the voice somewhere in the call
    bool third = false;  // the third-of-pitch wave is in the blend somewhere in the call
    bool fold = false;   // DRIVE sits above 0 somewhere in the call
  };

  void passThrough(const float* in, float* out, int n) {
    if (!idle_) {
      voice_.reset();
      fold_.clear();
      hp_.clear();
      sweep_.reset();
      phase_ = 0;
      rng_ = growl_detail::kJitterSeed;
      periodDepth_ = 1.0f;
      depthFade_ = 0.0f;
      fresh_ = true;
      idle_ = true;
    }
    if (out != in) std::memmove(out, in, sizeof(float) * static_cast<size_t>(n));
  }

  void applyTuning(const GrowlTuning& t) {
    if (tuningSet_ && std::memcmp(&t, &tuning_, sizeof t) == 0) return;
    tuning_ = t;
    tuningSet_ = true;
    fold_.setTuning(t);
    hp_.setHighPass(t.hpHz, kButterworthQ);
    trim_ = dbToGainFast(clampf(t.trimDb, -growl_detail::kMaxTrimDb, growl_detail::kMaxTrimDb));
    raspAt_ = -1.0f;
  }

  // Steps the knobs, the sweep, the fades and the wave's rate, prepares the voice and settles the call's
  // gains. `in` is read here, before the call writes its output.
  Call beginCall(const GrowlParams& p, const PitchFx& pitch, const float* in, int n, float wetTarget) {
    const GrowlTuning& t = tuning_;
    const PitchResult& pr = pitch.pitch();
    const float a = smoothCoefFor(n);
    const float growlKnob = smooth::clamp01(p.growl);
    const float driveKnob = smooth::clamp01(p.drive);
    const float mixKnob = smooth::clamp01(p.mix);
    const bool first = fresh_;
    if (fresh_) {
      growl_ = growlKnob;
      drive_ = driveKnob;
      mix_ = mixKnob;
      fresh_ = false;
    }
    const float inv = 1.0f / static_cast<float>(n);
    const float ramp = growl_detail::kFadeStep * static_cast<float>(n);
    Call c;
    c.wet0 = wet_;
    wet_ = toward(wet_, wetTarget, ramp);
    c.wetStep = (wet_ - c.wet0) * inv;

    // GROWL glides, and with the sweep it sets the throat, which glides on to the voice.
    const float growl0 = growl_;
    growl_ = smooth::step(growl_, growlKnob, a);
    const float sweep = sweep_.step(in, n, t.sensitivity, t.decayMs);
    const float rest = clampf(t.formantSt, -growl_detail::kMaxSt, growl_detail::kMaxSt);
    const float dropTop = clampf(t.throatDropTop, -growl_detail::kMaxThroatDropSt, growl_detail::kMaxThroatDropSt);
    throatTarget_ = clampf(rest + sweep * dropTop * growl_, -growl_detail::kMaxSt, growl_detail::kMaxSt);
    throat_ = first ? throatTarget_ : smooth::step(throat_, throatTarget_, a);

    // The voice, and the wave's rate while the tracker hears a pitch.
    const float corr = pitch.correctionSemis();
    const float drop = clampf(t.pitchSt, -growl_detail::kMaxSt, growl_detail::kMaxSt);
    const float voiceLag = static_cast<float>(PsolaVoice::kGrainDelay) + PitchTracker::clampPeriod(heldPeriod_);
    pitchRatio_ = exp2Fast((drop + corr) * (1.0f / 12.0f));
    voice_.anchorPulse(exp2Fast(rest * (1.0f / 12.0f)));
    voice_.prepare(heldPeriod_, pitchRatio_, exp2Fast(throat_ * (1.0f / 12.0f)), voiceLag, n);

    // The dry reads at the lag the voice plays at, sliding there from where it was.
    const float lag0 = dryLag_;
    const float lagWant = formant_detail::clampLag(voiceLag, heldPeriod_);
    dryLag_ = first ? lagWant : toward(dryLag_, lagWant, growl_detail::kDryLagSlew * static_cast<float>(n));
    c.lag0 = first ? dryLag_ : lag0;
    c.lagStep = (dryLag_ - c.lag0) * inv;
    if (pr.voiced && pr.period > 0.0f) {
      const float corrRatio = corr == 0.0f ? 1.0f : exp2Fast(corr * (1.0f / 12.0f));
      phaseStep_ = static_cast<uint32_t>(growl_detail::kSixthTurn * corrRatio / PitchTracker::clampPeriod(pr.period));
    }

    // The rasp: the depth fades with the pitch.
    const float fade0 = depthFade_;
    depthFade_ = toward(depthFade_, pr.voiced ? 1.0f : 0.0f, ramp);
    c.rasp0 = raspAt(growl0, fade0);
    const RaspGains r1 = raspAt(growl_, depthFade_);
    c.raspStep = {(r1.half - c.rasp0.half) * inv, (r1.third - c.rasp0.third) * inv, (r1.base - c.rasp0.base) * inv,
                  (r1.swing - c.rasp0.swing) * inv, (r1.jitter - c.rasp0.jitter) * inv};
    c.rasp = c.rasp0.swing != 0.0f || r1.swing != 0.0f;
    c.third = c.rasp0.third != 0.0f || r1.third != 0.0f;

    // The fold: DRIVE glides, and at 0 at both ends the fold is out.
    const float drive0 = drive_;
    drive_ = smooth::step(drive_, driveKnob, a);
    c.fold = drive0 > 0.0f || drive_ > 0.0f;
    if (c.fold) {
      c.fold0 = fold_.gainsAt(drive0);
      const FoldGains f1 = fold_.gainsAt(drive_);
      c.foldStep = {(f1.lowShelf - c.fold0.lowShelf) * inv, (f1.pre - c.fold0.pre) * inv,
                    (f1.peak - c.fold0.peak) * inv, (f1.post - c.fold0.post) * inv,
                      (f1.bias - c.fold0.bias) * inv};
    } else {
      fold_.clear();
    }

    // MIX glides, and its pair of gains splits the voice and the dry. Settled at 100 % the dry is out.
    const float mix0 = mix_;
    mix_ = smooth::step(mix_, mixKnob, a);
    c.dry = mix0 < 1.0f || mix_ < 1.0f;
    const MixGains m0 = mixGainsAt(mix0);
    const MixGains m1 = mixGainsAt(mix_);
    c.voiceGain0 = m0.voice;
    c.voiceGainStep = (m1.voice - m0.voice) * inv;
    c.dryGain0 = m0.dry;
    c.dryGainStep = (m1.dry - m0.dry) * inv;
    return c;
  }

  // The equal-power pair at MIX x. The ends are exactly 1 and 0, so MIX 100 % is the plain voice.
  struct MixGains {
    float voice;
    float dry;
  };

  static MixGains mixGainsAt(float x) {
    if (x >= 1.0f) return {1.0f, 0.0f};
    if (x <= 0.0f) return {0.0f, 1.0f};
    MixGains g;
    sinCosFast(x * kHalfPi, g.voice, g.dry);
    return g;
  }

  // The wave's gains at GROWL g and depth fade f. The level gain is 1 / sqrt(E[(base + swing m)^2]).
  // E[m] is 1/2 for either wave, so at depth D the gain's mean square before the level gain is
  // 1 - D + D^2 E[m^2]. The two waves share no harmonic, so E[m2 m3] is 1/4. A period's depth is
  // D = d u with u = 1 - r jitter and r uniform in 0..1, so E[D] = d (1 - jitter / 2) and
  // E[D^2] = d^2 (1 - jitter + jitter^2 / 3). libm runs only when g or f moves.
  RaspGains raspAt(float g, float f) {
    if (g == raspAt_ && f == raspFadeAt_) return rasp_;
    raspAt_ = g;
    raspFadeAt_ = f;
    const GrowlTuning& t = tuning_;
    const float slope = 1.0f + clampf(t.hardTop, 0.0f, growl_detail::kMaxHardTop) * g * g;
    const float over = (g - growl_detail::kThirdFrom) / (1.0f - growl_detail::kThirdFrom);
    const float b = clampf(t.thirdTop, 0.0f, 1.0f) * smooth::clamp01(over);
    const float q = 0.25f + 0.25f * growl_detail::clippedSinePower(slope);  // E[m^2] of either wave
    const float power = (1.0f - b) * (1.0f - b) * q + b * b * q + 2.0f * b * (1.0f - b) * 0.25f;
    const float d = g * f;
    const float j = clampf(t.jitterTop, 0.0f, 1.0f) * smooth::clamp01(g * (1.0f / kGrowlRaspTop));
    const float meanU = 1.0f - 0.5f * j;
    const float meanU2 = 1.0f - j + j * j * (1.0f / 3.0f);
    const float level = d > 0.0f ? 1.0f / sqrtf(1.0f - d * meanU + d * d * meanU2 * power) : 1.0f;
    rasp_ = {0.5f * slope, b, level * (1.0f - d), level * d, j};
    return rasp_;
  }

  // The next r, uniform in [0, 1), from a 32-bit LCG. Its top 24 bits are the draw.
  float nextUnit() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
  }

  // The rasp over the chunk at `at` in the call, in place on buf_. With no depth anywhere in the call
  // the wave only keeps its phase moving.
  void raspChunk(const Call& c, int at, int m) {
    if (!c.rasp) {
      phase_ += phaseStep_ * static_cast<uint32_t>(m);
      return;
    }
    if (c.third) raspLoop<true>(c, at, m);
    else raspLoop<false>(c, at, m);
  }

  template <bool Third>
  void raspLoop(const Call& c, int at, int m) {
    const float k = static_cast<float>(at);
    float half = c.rasp0.half + c.raspStep.half * k;
    float third = c.rasp0.third + c.raspStep.third * k;
    float base = c.rasp0.base + c.raspStep.base * k;
    float swing = c.rasp0.swing + c.raspStep.swing * k;
    float jitter = c.rasp0.jitter + c.raspStep.jitter * k;
    float depth = periodDepth_;
    uint32_t phase = phase_;
    const uint32_t step = phaseStep_;

    // The wave's offset from its crest. It wraps from near 2^32 to near 0 as the wave crosses the crest,
    // which is where a period ends and the next one draws its depth.
    uint32_t sinceCrest = phase * 3u - growl_detail::kCrestTurn;
    for (int i = 0; i < m; ++i) {
      half += c.raspStep.half;
      base += c.raspStep.base;
      swing += c.raspStep.swing;
      jitter += c.raspStep.jitter;
      phase += step;
      const uint32_t crest = phase * 3u - growl_detail::kCrestTurn;
      if (crest < sinceCrest) depth = 1.0f - nextUnit() * jitter;
      sinceCrest = crest;
      float w = fminf(fmaxf(0.5f + half * sinTurns(phase * 3u), 0.0f), 1.0f);
      if (Third) {
        third += c.raspStep.third;
        const float w3 = fminf(fmaxf(0.5f + half * sinTurns(phase * 2u), 0.0f), 1.0f);
        w += third * (w3 - w);
      }
      const size_t j = static_cast<size_t>(i);
      buf_[j] *= base + swing * (1.0f - depth * (1.0f - w));
    }
    phase_ = phase;
    periodDepth_ = depth;
  }

  // The wet path's high-pass over the first m samples of buf_, in place.
  void highPass(int m) {
    Biquad hp = hp_;
    for (int i = 0; i < m; ++i) {
      float& v = buf_[static_cast<size_t>(i)];
      v = hp.run(v);
    }
    hp_ = hp;
  }

  // The fold's gains at sample `at` of the call.
  static FoldGains foldAt(const Call& c, int at) {
    const float k = static_cast<float>(at);
    return {c.fold0.lowShelf + c.foldStep.lowShelf * k, c.fold0.pre + c.foldStep.pre * k,
            c.fold0.peak + c.foldStep.peak * k, c.fold0.post + c.foldStep.post * k,
            c.fold0.bias + c.foldStep.bias * k};
  }

  // Blends the dry into buf_ in place, trim included: the voice at its MIX share plus the ring read at
  // the lag at its own. `head` is the ring index of the chunk's first sample.
  void blendDry(const Call& c, const VoiceRing& ring, int head, int m, int at) {
    const float k = static_cast<float>(at);
    float voiceGain = c.voiceGain0 + c.voiceGainStep * k;
    float dryGain = c.dryGain0 + c.dryGainStep * k;
    float lag = c.lag0 + c.lagStep * k;
    for (int i = 0; i < m; ++i) {
      voiceGain += c.voiceGainStep;
      dryGain += c.dryGainStep;
      lag += c.lagStep;
      const float dry = readRing<kVoiceRingLen>(ring.data(), wrapOnce<kVoiceRingLen>(head + i), lag);
      float& v = buf_[static_cast<size_t>(i)];
      v = trim_ * voiceGain * v + dryGain * dry;
    }
  }

  // The crossfade from the card's input to the trimmed, mixed voice. Settled on, the input is gone.
  void mixChunk(const Call& c, const float* in, float* out, int m, int at) {
    const float trim = c.dry ? 1.0f : trim_;  // blendDry has applied it
    if (c.wet0 == 1.0f && c.wetStep == 0.0f) {
      for (int i = 0; i < m; ++i) out[i] = trim * buf_[static_cast<size_t>(i)];
      return;
    }
    float w = c.wet0 + c.wetStep * static_cast<float>(at);
    for (int i = 0; i < m; ++i) {
      w += c.wetStep;
      const float x = in[i];
      out[i] = x + w * (trim * buf_[static_cast<size_t>(i)] - x);
    }
  }

  // x moved toward target by at most step; it lands on the target exactly.
  static float toward(float x, float target, float step) {
    if (x < target) return target - x <= step ? target : x + step;
    return x - target <= step ? target : x - step;
  }

  // The 20 ms one-pole's coefficient for one step over n samples, kept for the last n.
  float smoothCoefFor(int n) {
    if (n != coefN_) {
      coef_ = smooth::blockCoef(smooth::kSmoothSec, n);
      coefN_ = n;
    }
    return coef_;
  }

  formant_detail::FormantPsolaVoice voice_;
  growl_detail::Fold fold_;
  Biquad hp_;  // the wet path's high-pass
  std::array<float, kBlock> buf_{};  // the voice's chunk, then the rasp and the fold in place
  GrowlTuning tuning_;
  bool tuningSet_ = false;
  float trim_ = 1.0f;
  SyllableSweep sweep_;        // reads the card's input
  float throat_ = 0.0f;        // the throat the voice plays after the glide, st
  float throatTarget_ = 0.0f;  // the throat the sweep asks for, st
  float heldPeriod_ = 0.0f;    // the tracker's period, or the last one it gave
  uint32_t phase_ = 0;         // the wave's counter, at a sixth of the pitch
  uint32_t phaseStep_ = 0;     // its step per sample, held through unvoiced frames
  uint32_t rng_ = growl_detail::kJitterSeed;  // the jitter generator
  float periodDepth_ = 1.0f;   // the running period's depth factor, 1 - r * jitter
  float pitchRatio_ = 0.5f;    // the voice's pitch ratio at the last call
  float wet_ = 0.0f;           // the dry-to-voice crossfade, 0 dry .. 1 voice
  float depthFade_ = 0.0f;     // the wave's depth fade, 1 with a pitch
  float growl_ = 0.0f;         // GROWL after its glide
  float drive_ = 0.0f;         // DRIVE after its glide
  float mix_ = 1.0f;           // MIX after its glide
  float dryLag_ = 0.0f;        // the delay the dry reads at, samples
  RaspGains rasp_;             // the wave's gains at raspAt_ and raspFadeAt_
  float raspAt_ = -1.0f;
  float raspFadeAt_ = -1.0f;
  float coef_ = 1.0f;
  int coefN_ = 0;
  bool fresh_ = true;  // the next call takes the knobs outright
  bool idle_ = true;   // the last call passed the input through
};

}  // namespace cv
