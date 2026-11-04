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

// The Growl card, the gross monster voice of SOPHIE's "Whole New World". It replaces the voice: once
// the card is up the dry signal is gone, and three parts run in a row.
//
// 1. The drop. A grain voice cut from the pitch stage's input ring on the tracker's period grid plays
//    the voice at a pitch ratio of 2^(pitchSt / 12) and a grain playback rate of 2^(formantSt / 12),
//    an octave down with the throat 5 st down. It follows Autotune's correction and reads at
//    Autotune's lag, where the tracker's frame lines up with the audio.
// 2. The rasp. A raised wave at half the tracked pitch multiplies the dropped voice, so alternate
//    periods of the sung note come out loud and quiet, the period doubling of a growl's false folds.
//    Over the top half of GROWL a wave at a third of the pitch blends in. The wave is a raised sine at
//    GROWL 0 and hardens toward a square as GROWL rises. A level gain holds the modulated voice at the
//    power of the plain one. Without a pitch (unvoiced, silence) the wave holds its last rate and its
//    depth falls to 0 over 20 ms, so breath does not buzz.
// 3. The fold. A low shelf ahead of Distortion's quadratic knee and a high shelf after it tilt the
//    tone to the low mids. DRIVE sets the knee's input gain and both shelves' depth, and a makeup gain
//    follows it. At DRIVE 0 the fold is out and the rasped voice passes clean. The knee runs at the
//    base rate, with no oversampling.
//
// The ring holds the input of the slot where the pitch stage runs, so the stages between that slot and
// this card are not in the growl voice.

namespace cv {

struct GrowlTuning {
  float pitchSt = -12.0f;       // the drop; the voice's pitch ratio is 2^(pitchSt / 12)
  float formantSt = -5.0f;      // the throat; the grains play at 2^(formantSt / 12)
  float hardTop = 7.0f;         // the wave's added slope at GROWL 100 %, 1 + hardTop * GROWL^2 times a raised sine's
  float thirdTop = 0.5f;        // the third-of-pitch wave's share at GROWL 100 %
  float lowShelfHz = 200.0f;    // ahead of the knee
  float lowShelfDb = 6.0f;      // at DRIVE 100 %
  float highShelfHz = 4000.0f;  // after the knee
  float highShelfDb = -6.0f;    // at DRIVE 100 %
  float driveDb = 30.0f;        // the knee's input gain at DRIVE 100 %
  float makeupSat = 10.0f;      // the knee's input gain where the makeup law flattens
  float railSoft = 0.1f;        // the knee's soft edge as a fraction of the rail
  float railAsym = 0.05f;       // the negative rail sits at -(1 - railAsym)
  float trimDb = 0.87f;         // level rule: Adam's three clips at GROWL 50 % and DRIVE 50 % land +0.25 dB
};

struct GrowlParams {
  bool on = false;
  float growl = 0.5f;  // knob A (GROWL), 0..1
  float drive = 0.5f;  // knob B (DRIVE), 0..1
  GrowlTuning tuning;
};

namespace growl_detail {

// The ranges the engine clamps the tuning to. The render tool refuses a value outside them.
constexpr float kMaxSt = 12.0f;
constexpr float kMaxHardTop = 32.0f;
constexpr float kMaxDriveDb = 48.0f;
constexpr float kMinMakeupSat = 1.0f;
constexpr float kMaxMakeupSat = 1000.0f;
constexpr float kMaxShelfDb = 24.0f;
constexpr float kMaxTrimDb = 24.0f;
constexpr float kMinRailSoft = 0.01f;
constexpr float kMaxRailAsym = 0.5f;

// The depth's fall without a pitch and the dry-to-voice crossfade both take 20 ms, in straight lines.
constexpr float kFadeMs = 20.0f;
constexpr float kFadeStep = 1.0f / (kFadeMs * kSmpPerMs);

// GROWL where the third-of-pitch wave starts to blend in.
constexpr float kThirdFrom = 0.5f;

// One turn of the wave's phase counter over six. The counter runs at a sixth of the tracked pitch, so
// three times it is the half-pitch wave and twice it the third-pitch wave, locked together.
constexpr float kSixthTurn = 4294967296.0f / 6.0f;

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
  float lowShelf = 0.0f;   // the low shelf's gain less 1
  float pre = 1.0f;        // the knee's input gain
  float highShelf = 0.0f;  // the high shelf's gain less 1
  float post = 1.0f;       // the makeup
};

// The rasp's wave and gains at one GROWL and depth fade: m = clamp(0.5 + half * sin, 0, 1) for each
// wave, the third's share of the blend, and y = voice * (base + swing * m).
struct RaspGains {
  float half = 0.5f;
  float third = 0.0f;
  float base = 1.0f;
  float swing = 0.0f;
};

// The fold: a first-order low shelf, the knee's input gain, Distortion's rail, a first-order high
// shelf, the makeup and a DC blocker for the rail's asymmetry. Its gains glide across a call from the
// last call's to this one's.
class Fold {
 public:
  void setTuning(const GrowlTuning& t) {
    lowLp_.set(t.lowShelfHz);
    highLp_.set(t.highShelfHz);
    const float soft = clampf(t.railSoft, kMinRailSoft, 1.0f);
    rail_ = Distortion::makeRail(1.0f - clampf(t.railAsym, 0.0f, kMaxRailAsym), soft);
    lowDb_ = clampf(t.lowShelfDb, -kMaxShelfDb, kMaxShelfDb);
    highDb_ = clampf(t.highShelfDb, -kMaxShelfDb, kMaxShelfDb);
    driveDb_ = clampf(t.driveDb, 0.0f, kMaxDriveDb);
    sat_ = clampf(t.makeupSat, kMinMakeupSat, kMaxMakeupSat);
    unityMakeup_ = makeup(1.0f);
    at_ = -1.0f;
  }

  // The gains at DRIVE d, worked out again only when d moves.
  const FoldGains& gainsAt(float d) {
    if (d != at_) {
      at_ = d;
      gains_.lowShelf = dbToGainFast(lowDb_ * d) - 1.0f;
      gains_.pre = dbToGainFast(driveDb_ * d);
      gains_.highShelf = dbToGainFast(highDb_ * d) - 1.0f;
      gains_.post = unityMakeup_ / makeup(gains_.pre);
    }
    return gains_;
  }

  // x through the fold in place over m samples, the gains running from g by step per sample.
  void run(float* x, int m, FoldGains g, const FoldGains& step) {
    const Distortion::Rail r = rail_;
    float lo = lowLp_.y;
    float hi = highLp_.y;
    float dc = dc_.y;
    const float aLo = lowLp_.a;
    const float aHi = highLp_.a;
    const float aDc = dc_.a;
    for (int i = 0; i < m; ++i) {
      g.lowShelf += step.lowShelf;
      g.pre += step.pre;
      g.highShelf += step.highShelf;
      g.post += step.post;
      const float v = x[i];
      lo += aLo * (v - lo);
      const float c = Distortion::rail(r, (v + g.lowShelf * lo) * g.pre);
      hi += aHi * (c - hi);
      const float y = (c + g.highShelf * (c - hi)) * g.post;
      dc += aDc * (y - dc);
      x[i] = y - dc;
    }
    lowLp_.y = lo;
    highLp_.y = hi;
    dc_.y = dc;
  }

  void clear() {
    lowLp_.clear();
    highLp_.clear();
    dc_.clear();
  }

  // False once a NaN or an infinity got into a filter, which no later sample would wash out.
  bool finite() const { return std::isfinite(lowLp_.y + highLp_.y + dc_.y); }

 private:
  // The knee's output level against its input gain: linear at first, flat once the gain passes sat_.
  float makeup(float gain) const {
    const float r = gain / sat_;
    return gain / sqrtf(1.0f + r * r);
  }

  OnePole lowLp_;
  OnePole highLp_;
  DcBlocker dc_;
  Distortion::Rail rail_ = Distortion::makeRail(0.95f, 0.1f);
  FoldGains gains_;
  float lowDb_ = 0.0f;
  float highDb_ = 0.0f;
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
    buf_.fill(0.0f);
    heldPeriod_ = 0.0f;
    phase_ = 0;
    phaseStep_ = 0;
    wet_ = 0.0f;
    depthFade_ = 0.0f;
    growl_ = 0.0f;
    drive_ = 0.0f;
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
    const Call c = beginCall(p, pitch, n, wetTarget);
    int first = pitch.head() - n;
    if (first < 0) first += kVoiceRingLen;
    for (int at = 0; at < n; at += kBlock) {
      const int m = n - at < kBlock ? n - at : kBlock;
      buf_.fill(0.0f);
      voice_.render(pitch.ring(), wrapOnce<kVoiceRingLen>(first + at), m, buf_.data());
      raspChunk(c, at, m);
      if (c.fold) fold_.run(buf_.data(), m, foldAt(c, at), c.foldStep);
      mixChunk(c, in + at, out + at, m, at);
    }
    if (!fold_.finite()) fold_.clear();
  }

  // Test hooks: the wave's depth fade, 1 with a pitch and 0 without, and the dry-to-voice crossfade.
  float depthFade() const { return depthFade_; }
  float wet() const { return wet_; }

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
    bool rasp = false;   // the wave moves the voice somewhere in the call
    bool third = false;  // the third-of-pitch wave is in the blend somewhere in the call
    bool fold = false;   // DRIVE sits above 0 somewhere in the call
  };

  void passThrough(const float* in, float* out, int n) {
    if (!idle_) {
      voice_.reset();
      fold_.clear();
      phase_ = 0;
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
    trim_ = dbToGainFast(clampf(t.trimDb, -growl_detail::kMaxTrimDb, growl_detail::kMaxTrimDb));
    rate_ = exp2Fast(clampf(t.formantSt, -growl_detail::kMaxSt, growl_detail::kMaxSt) * (1.0f / 12.0f));
    raspAt_ = -1.0f;
  }

  // Steps the knobs, the fades and the wave's rate, prepares the voice and settles the call's gains.
  Call beginCall(const GrowlParams& p, const PitchFx& pitch, int n, float wetTarget) {
    const GrowlTuning& t = tuning_;
    const PitchResult& pr = pitch.pitch();
    const float a = smoothCoefFor(n);
    const float growlKnob = smooth::clamp01(p.growl);
    const float driveKnob = smooth::clamp01(p.drive);
    if (fresh_) {
      growl_ = growlKnob;
      drive_ = driveKnob;
      fresh_ = false;
    }
    const float inv = 1.0f / static_cast<float>(n);
    const float ramp = growl_detail::kFadeStep * static_cast<float>(n);
    Call c;
    c.wet0 = wet_;
    wet_ = toward(wet_, wetTarget, ramp);
    c.wetStep = (wet_ - c.wet0) * inv;

    // The voice, and the wave's rate while the tracker hears a pitch.
    const float corr = pitch.correctionSemis();
    const float drop = clampf(t.pitchSt, -growl_detail::kMaxSt, growl_detail::kMaxSt);
    voice_.prepare(heldPeriod_, exp2Fast((drop + corr) * (1.0f / 12.0f)), rate_,
                   static_cast<float>(PsolaVoice::kGrainDelay) + PitchTracker::clampPeriod(heldPeriod_), n);
    if (pr.voiced && pr.period > 0.0f) {
      const float corrRatio = corr == 0.0f ? 1.0f : exp2Fast(corr * (1.0f / 12.0f));
      phaseStep_ = static_cast<uint32_t>(growl_detail::kSixthTurn * corrRatio / PitchTracker::clampPeriod(pr.period));
    }

    // The rasp: GROWL glides, and the depth fades with the pitch.
    const float growl0 = growl_;
    growl_ = smooth::step(growl_, growlKnob, a);
    const float fade0 = depthFade_;
    depthFade_ = toward(depthFade_, pr.voiced ? 1.0f : 0.0f, ramp);
    c.rasp0 = raspAt(growl0, fade0);
    const RaspGains r1 = raspAt(growl_, depthFade_);
    c.raspStep = {(r1.half - c.rasp0.half) * inv, (r1.third - c.rasp0.third) * inv, (r1.base - c.rasp0.base) * inv,
                  (r1.swing - c.rasp0.swing) * inv};
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
                    (f1.highShelf - c.fold0.highShelf) * inv, (f1.post - c.fold0.post) * inv};
    } else {
      fold_.clear();
    }
    return c;
  }

  // The wave's gains at GROWL g and depth fade f. The level gain is 1 / sqrt(E[(base + swing m)^2]).
  // E[m] is 1/2 for either wave, so at depth D the gain's mean square before the level gain is
  // 1 - D + D^2 E[m^2]. The two waves share no harmonic, so E[m2 m3] is 1/4. libm runs only when g or
  // f moves.
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
    const float level = d > 0.0f ? 1.0f / sqrtf(1.0f - d + d * d * power) : 1.0f;
    rasp_ = {0.5f * slope, b, level * (1.0f - d), level * d};
    return rasp_;
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
    uint32_t phase = phase_;
    const uint32_t step = phaseStep_;
    for (int i = 0; i < m; ++i) {
      half += c.raspStep.half;
      base += c.raspStep.base;
      swing += c.raspStep.swing;
      phase += step;
      float w = fminf(fmaxf(0.5f + half * sinTurns(phase * 3u), 0.0f), 1.0f);
      if (Third) {
        third += c.raspStep.third;
        const float w3 = fminf(fmaxf(0.5f + half * sinTurns(phase * 2u), 0.0f), 1.0f);
        w += third * (w3 - w);
      }
      const size_t j = static_cast<size_t>(i);
      buf_[j] *= base + swing * w;
    }
    phase_ = phase;
  }

  // The fold's gains at sample `at` of the call.
  static FoldGains foldAt(const Call& c, int at) {
    const float k = static_cast<float>(at);
    return {c.fold0.lowShelf + c.foldStep.lowShelf * k, c.fold0.pre + c.foldStep.pre * k,
            c.fold0.highShelf + c.foldStep.highShelf * k, c.fold0.post + c.foldStep.post * k};
  }

  // The crossfade from the dry input to the trimmed voice. Settled on, the dry is gone.
  void mixChunk(const Call& c, const float* in, float* out, int m, int at) {
    if (c.wet0 == 1.0f && c.wetStep == 0.0f) {
      for (int i = 0; i < m; ++i) out[i] = trim_ * buf_[static_cast<size_t>(i)];
      return;
    }
    float w = c.wet0 + c.wetStep * static_cast<float>(at);
    for (int i = 0; i < m; ++i) {
      w += c.wetStep;
      const float x = in[i];
      out[i] = x + w * (trim_ * buf_[static_cast<size_t>(i)] - x);
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
  std::array<float, kBlock> buf_{};  // the voice's chunk, then the rasp and the fold in place
  GrowlTuning tuning_;
  bool tuningSet_ = false;
  float trim_ = 1.0f;
  float rate_ = 1.0f;          // the grains' playback rate
  float heldPeriod_ = 0.0f;    // the tracker's period, or the last one it gave
  uint32_t phase_ = 0;         // the wave's counter, at a sixth of the pitch
  uint32_t phaseStep_ = 0;     // its step per sample, held through unvoiced frames
  float wet_ = 0.0f;           // the dry-to-voice crossfade, 0 dry .. 1 voice
  float depthFade_ = 0.0f;     // the wave's depth fade, 1 with a pitch
  float growl_ = 0.0f;         // GROWL after its glide
  float drive_ = 0.0f;         // DRIVE after its glide
  RaspGains rasp_;             // the wave's gains at raspAt_ and raspFadeAt_
  float raspAt_ = -1.0f;
  float raspFadeAt_ = -1.0f;
  float coef_ = 1.0f;
  int coefN_ = 0;
  bool fresh_ = true;  // the next call takes the knobs outright
  bool idle_ = true;   // the last call passed the input through
};

}  // namespace cv
