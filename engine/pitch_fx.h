#pragma once

#include <array>
#include <cmath>

#include "engine/autotune.h"
#include "engine/common.h"
#include "engine/harmony.h"
#include "engine/octave.h"
#include "engine/octave_b.h"
#include "engine/octave_c.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// Shared pitch front end. Harmony and Octave both track and shift the dry
// input, so neither tracker ever hears the other's voices. Each stage has its
// own equal-power MIX; dry gain is the product of the two. Autotune, when on,
// replaces the dry voice with the corrected one, and Harmony and Octave add
// its correction to their targets.

namespace cv {

struct PitchFxParams {
  AutotuneParams autotune;
  HarmonyParams harmony;
  OctaveParams octave;
};

class PitchFx {
 public:
  PitchFx() { reset(); }

  void reset() {
    ring_.fill(0.0f);
    writeCount_ = 0;
    tracker_.reset();
    autotune_.reset();
    harmony_.reset();
    octaveA_.reset();
    octaveB_.reset();
    octaveC_.reset();
    lp_.fill(0.0f);
    lpZ_ = 0.0f;
    harmonyLp_.fill(0.0f);
    harmonyLpZ_ = 0.0f;
    autotuneLp_.fill(0.0f);
    autotuneLpZ_ = 0.0f;
    engine_ = 0;
    mix_.fill(0.0f);
    active_.fill(0.0f);
    activeAt_ = 0.0f;
    gains_ = Gains{};
    autotuneWas_ = false;
    harmonyWas_ = false;
    octaveWas_ = false;
    lpWas_ = false;
    harmonyLpWas_ = false;
    autotuneLpWas_ = false;
    fresh_ = true;
  }

  void process(const float* in, float* out, int n, const PitchFxParams& p) {
    // Switching engines resets the one left behind so it starts clean next time.
    const int engine = p.octave.engine == 1 || p.octave.engine == 2 ? p.octave.engine : 0;
    if (engine != engine_) {
      if (engine != 0) octaveA_.reset();
      if (engine != 1) octaveB_.reset();
      if (engine != 2) octaveC_.reset();
      engine_ = engine;
    }

    // The tracker's buffers fill every block; it analyses only for a consumer.
    // Octave C shifts without a pitch.
    const bool trackerOn = p.autotune.on || p.harmony.on || (p.octave.on && engine_ != 2);
    tracker_.push(in, n, p.harmony.tuning.voicedThreshold, trackerOn);
    const PitchResult& pr = tracker_.result();

    // A card coming on starts its voice clean rather than from stale grains.
    if (p.autotune.on && !autotuneWas_) autotune_.reset();
    autotuneWas_ = p.autotune.on;
    const float corr = autotune_.prepare(pr, p.autotune);
    const float activeAtT = p.autotune.on ? 1.0f : 0.0f;

    bool harmonyOn = harmony_.prepare(pr, p.harmony, corr);
    if (harmonyOn && !harmonyWas_) {
      harmony_.reset();
      harmonyOn = harmony_.prepare(pr, p.harmony, corr);
    }
    harmonyWas_ = harmonyOn;
    bool octaveOn = prepareOctave(pr, p.octave, corr);
    if (octaveOn && !octaveWas_) {
      resetOctave();
      octaveOn = prepareOctave(pr, p.octave, corr);
    }
    octaveWas_ = octaveOn;

    // Each low-passed ring is kept only while the engine that reads it is
    // selected; it starts from silence when that engine is picked.
    const bool lpOn = engine_ == 1;
    const bool harmonyLpOn = p.harmony.engine == 1;
    const bool autotuneLpOn = p.autotune.engine != 0;
    if (lpOn && !lpWas_) {
      lp_.fill(0.0f);
      lpZ_ = 0.0f;
    }
    if (harmonyLpOn && !harmonyLpWas_) {
      harmonyLp_.fill(0.0f);
      harmonyLpZ_ = 0.0f;
    }
    if (autotuneLpOn && !autotuneLpWas_) {
      autotuneLp_.fill(0.0f);
      autotuneLpZ_ = 0.0f;
    }
    lpWas_ = lpOn;
    harmonyLpWas_ = harmonyLpOn;
    autotuneLpWas_ = autotuneLpOn;
    const float lpA = lpOn ? lpCoef(p.octave.tuning.epochLpHz) : 0.0f;
    const float harmonyLpA = harmonyLpOn ? lpCoef(p.harmony.tuning.shifter.epochLpHz) : 0.0f;
    const float autotuneLpA = autotuneLpOn ? lpCoef(p.autotune.tuning.shifter.epochLpHz) : 0.0f;

    const std::array<float, kStages> mixT = {smooth::clamp01(p.harmony.mix),
                                             smooth::clamp01(p.octave.mix)};
    const std::array<float, kStages> activeT = {harmonyOn ? 1.0f : 0.0f, octaveOn ? 1.0f : 0.0f};
    if (fresh_) {
      mix_ = mixT;
      active_ = activeT;
      activeAt_ = activeAtT;
      gains_ = gainsNow();
      fresh_ = false;
    }

    // Every stage settled off: the dry gain is exactly 1, so only the rings
    // need writing. MIX can snap since nothing reads it until a stage comes on.
    const bool idle = activeAt_ == 0.0f && activeAtT == 0.0f && active_[0] == 0.0f &&
                      activeT[0] == 0.0f && active_[1] == 0.0f && activeT[1] == 0.0f;
    if (idle) {
      mix_ = mixT;
      gains_ = gainsNow();
      for (int i = 0; i < n; ++i) {
        writeRings(in[i], lpOn, lpA, harmonyLpOn, harmonyLpA, autotuneLpOn, autotuneLpA);
        out[i] = in[i];
        writeCount_ = writeCount_ + 1 == kVoiceRingLen ? 0 : writeCount_ + 1;
      }
      return;
    }

    // The smoothers still step once per sample, so their path and snap points
    // are unchanged; the gains they set are evaluated at the block edges and
    // interpolated between, which is exact once settled.
    const float a = smooth::kSmoothCoef;
    const Gains g0 = gains_;
    for (int k = 0; k < n; ++k) {
      activeAt_ = smooth::step(activeAt_, activeAtT, a);
      for (int s = 0; s < kStages; ++s) {
        mix_[s] = smooth::step(mix_[s], mixT[s], a);
        active_[s] = smooth::step(active_[s], activeT[s], a);
      }
    }
    gains_ = gainsNow();
    const Gains g1 = gains_;
    const float inv = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      writeRings(in[i], lpOn, lpA, harmonyLpOn, harmonyLpA, autotuneLpOn, autotuneLpA);
      const float harm = harmony_.tick(ring_, harmonyLp_, writeCount_, pr.period);
      const float oct = tickOctave(pr.period);
      const float f = static_cast<float>(i + 1) * inv;
      const float at = g0.at + (g1.at - g0.at) * f;
      const float base =
          at > 0.0f ? at * autotune_.tick(ring_, autotuneLp_, writeCount_, pr.period) +
                          (1.0f - at) * in[i]
                    : in[i];
      const float dry = g0.dry + (g1.dry - g0.dry) * f;
      const float wetH = g0.wetH + (g1.wetH - g0.wetH) * f;
      const float wetO = g0.wetO + (g1.wetO - g0.wetO) * f;
      out[i] = dry * base + wetH * harm + wetO * oct;
      // Voices only use writeCount modulo the ring, so wrapping here is exact.
      writeCount_ = writeCount_ + 1 == kVoiceRingLen ? 0 : writeCount_ + 1;
    }
  }

  const PitchResult& pitch() const { return tracker_.result(); }

  // Autotune's smoothed correction in semitones.
  float correctionSemis() const { return autotune_.correctionSemis(); }

 private:
  // One-pole coefficient for an option B peak-search low-pass.
  static float lpCoef(float hz) {
    hz = hz < kMinLpHz ? kMinLpHz : (hz > kMaxLpHz ? kMaxLpHz : hz);
    return 1.0f - expf(-kTwoPi * hz / static_cast<float>(kSampleRate));
  }

  bool prepareOctave(const PitchResult& pr, const OctaveParams& o, float corr) {
    if (engine_ == 1) return octaveB_.prepare(pr, o, corr);
    if (engine_ == 2) return octaveC_.prepare(pr, o, corr);
    return octaveA_.prepare(pr, o, corr);
  }

  // Equal-power dry/wet gains at the smoothers' current values. Dry is the
  // product over stages; a stage that is off contributes exactly 1 and 0.
  struct Gains {
    float dry = 1.0f;
    float wetH = 0.0f;
    float wetO = 0.0f;
    float at = 0.0f;
  };

  Gains gainsNow() const {
    Gains g;
    g.at = activeAt_;
    const float dH = 1.0f + (cosf(mix_[0] * kHalfPi) - 1.0f) * active_[0];
    const float dO = 1.0f + (cosf(mix_[1] * kHalfPi) - 1.0f) * active_[1];
    g.dry = dH * dO;
    g.wetH = sinf(mix_[0] * kHalfPi) * active_[0];
    g.wetO = sinf(mix_[1] * kHalfPi) * active_[1];
    return g;
  }

  void resetOctave() {
    if (engine_ == 1) octaveB_.reset();
    else if (engine_ == 2) octaveC_.reset();
    else octaveA_.reset();
  }

  // The shared ring always takes the sample; each low-passed copy only while on.
  void writeRings(float x, bool lpOn, float lpA, bool harmonyLpOn, float harmonyLpA,
                  bool autotuneLpOn, float autotuneLpA) {
    ring_[writeCount_] = x;
    if (lpOn) {
      lpZ_ += lpA * (x - lpZ_);
      lp_[writeCount_] = lpZ_;
    }
    if (harmonyLpOn) {
      harmonyLpZ_ += harmonyLpA * (x - harmonyLpZ_);
      harmonyLp_[writeCount_] = harmonyLpZ_;
    }
    if (autotuneLpOn) {
      autotuneLpZ_ += autotuneLpA * (x - autotuneLpZ_);
      autotuneLp_[writeCount_] = autotuneLpZ_;
    }
  }

  float tickOctave(float period) {
    if (engine_ == 1) return octaveB_.tick(ring_, lp_, writeCount_, period);
    if (engine_ == 2) return octaveC_.tick(ring_, writeCount_);
    return octaveA_.tick(ring_, writeCount_, period);
  }

  static constexpr int kStages = 2;  // 0 harmony, 1 octave
  static constexpr float kHalfPi = 1.57079632679489661923f;
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static constexpr float kMinLpHz = 100.0f;
  static constexpr float kMaxLpHz = 4000.0f;

  VoiceRing ring_{};
  PitchTracker tracker_;
  AutotuneVoice autotune_;
  HarmonyVoices harmony_;
  OctaveVoice octaveA_;
  OctaveVoiceB octaveB_;
  OctaveVoiceC octaveC_;
  VoiceRing lp_{};  // ring_ low-passed for Octave B's peak search
  float lpZ_ = 0.0f;
  VoiceRing harmonyLp_{};  // the same at Harmony's own cutoff, for Harmony B
  float harmonyLpZ_ = 0.0f;
  VoiceRing autotuneLp_{};  // the same at Autotune's cutoff, for Autotune B
  float autotuneLpZ_ = 0.0f;
  int engine_ = 0;
  std::array<float, kStages> mix_{};
  std::array<float, kStages> active_{};
  float activeAt_ = 0.0f;  // Autotune crossfade, 0 dry .. 1 corrected
  Gains gains_;            // gains at the end of the last block
  long writeCount_ = 0;
  bool autotuneWas_ = false;  // last block's on-state per card, for enable edges
  bool harmonyWas_ = false;
  bool octaveWas_ = false;
  bool lpWas_ = false;  // last block's selection per low-passed ring
  bool harmonyLpWas_ = false;
  bool autotuneLpWas_ = false;
  bool fresh_ = true;
};

}  // namespace cv
