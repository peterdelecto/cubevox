#pragma once

#include <array>
#include <cmath>
#include <limits>

#include "engine/autotune.h"
#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/formant_voice.h"
#include "engine/octave.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// Shared pitch front end. The tracker hears only the dry input. Autotune, when
// on, crossfades from the dry voice to the corrected one once the tracker has a
// period, and Octave adds Autotune's correction to its interval. The Formant card
// adds a second voice that follows the same correction and lags the input by the
// same amount as Autotune's, so the two line up. It mixes with the base (the dry
// voice, or Autotune's crossfade) ahead of Octave. Every crossfade and MIX is
// equal-power. A voice resets on its enable edge only once its crossfade has
// settled at zero, so a quick off and on keeps the running voice instead of
// cutting to fresh grains. The tracker's frame is warm only while the stage runs.
// The chain resets the stage when every pitch card is off, so the first voiced
// frame after such a switch-on comes about 32 ms in, where a running stage gives
// it in 8 ms.

namespace cv {

// The panel's SHIFT range, whole semitones.
constexpr int kFormantMaxShiftSt = 7;

struct FormantTuning {
  float trimDb = 1.92f;  // level rule at SHIFT -3, MIX 100 %: the shifted voice alone lands 0 to +0.5 dB
};

struct FormantParams {
  bool on = false;
  int shiftSt = -3;   // knob A (SHIFT), -7..+7 st; the grains play at 2^(shiftSt / 12)
  float mix = 1.0f;   // knob B (MIX), 0..1
  FormantTuning tuning;
};

struct PitchFxParams {
  AutotuneParams autotune;
  OctaveParams octave;
  FormantParams formant;
  bool crowdOn = false;  // the Crowd card reads the ring and the tracker, so both run
};

class PitchFx {
  static_assert(PitchTracker::kReplaySpan < kVoiceRingLen, "the ring holds a tracker replay's history");

 public:
  PitchFx() {
    formant_.latchGain(true);
    reset();
  }

  void reset() {
    ring_.fill(0.0f);
    head_ = 0;
    tracker_.reset();
    autotune_.reset();
    octave_.reset();
    formant_.reset();
    octaveMix_ = 0.0f;
    octaveFade_ = 0.0f;
    autotuneFade_ = 0.0f;
    formantMix_ = 0.0f;
    formantFade_ = 0.0f;
    shiftSet_ = false;
    gains_ = Gains{};
    autotuneWas_ = false;
    octaveWas_ = false;
    formantWas_ = false;
    fresh_ = true;
  }

  void process(const float* in, float* out, int n, const PitchFxParams& p) {
    // The tracker runs while Autotune or Formant is on or still fading, so the voice keeps its period
    // through the crossfade, and while the Crowd card is on. Octave shifts without a pitch, so with no
    // reader the tracker only keeps hop time, and its next start replays its history from the ring.
    // The 48 kHz fine pass runs for Autotune and Formant, whose voices must sit on the sung pitch to
    // the cent; the Crowd card's copies drift by more than the coarse lag misses.
    const bool autotune = p.autotune.on || autotuneFade_ > 0.0f;
    const bool formant = p.formant.on || formantFade_ > 0.0f;
    if (autotune || formant || p.crowdOn) {
      tracker_.push(in, n, autotune || formant, PitchTracker::History{ring_.data(), kVoiceRingLen, head_});
    } else {
      tracker_.idle(n);
    }
    const Targets t = prepareVoices(p);
    if (fresh_) {
      octaveMix_ = t.octaveMix;
      octaveFade_ = t.octaveFade;
      autotuneFade_ = t.autotuneFade;
      formantMix_ = t.formantMix;
      formantFade_ = t.formantFade;
      gains_ = gainsNow();
      fresh_ = false;
    }
    if (settledOff(t)) {
      passThrough(in, out, n, t);
      return;
    }
    mixBlock(in, out, n, t, p.formant);
  }

  const PitchResult& pitch() const { return tracker_.result(); }

  // Autotune's smoothed correction in semitones. It is the live value while Autotune is on, runs
  // down with the crossfade after Autotune goes off, and is exactly 0 once the crossfade settles.
  // The crossfade takes about 280 ms to settle, so the scaling lets a reader follow the dry voice
  // back instead of holding the old correction until then.
  float correctionSemis() const {
    const float live = autotune_.correctionSemis();
    return autotuneWas_ ? live : live * autotuneFade_;
  }

  // The input ring and the index the next sample writes to (already wrapped). After process(),
  // the block's last sample sits at head() - 1.
  const VoiceRing& ring() const { return ring_; }
  int head() const { return head_; }

  // The Formant crossfade, 0 off .. 1 on, for the tests.
  float formantFade() const { return formantFade_; }

 private:
  // Where the smoothers head this block.
  struct Targets {
    float octaveMix;       // Octave MIX
    float octaveFade;      // Octave on, 0 or 1
    float autotuneFade;    // Autotune on with a period, 0 or 1
    float formantMix;      // Formant MIX
    float formantFade;     // Formant on with a period, 0 or 1
  };

  // Equal-power gains at the smoothers' current values: Octave's dry and wet, the crossfade from
  // the dry input to Autotune's corrected voice, and Formant's share of the base and of the shifted
  // voice. A stage that is off contributes exactly 1 and 0.
  struct Gains {
    float dry = 1.0f;
    float wet = 0.0f;
    float autotune = 0.0f;     // the corrected voice's share of the crossfade
    float autotuneDry = 1.0f;  // the dry input's share
    float formantBase = 1.0f;  // the base's share after Formant's mix
    float formantWet = 0.0f;   // the shifted voice's share, trim included
    float formantTrim = 1.0f;  // the trim formantWet carries
  };

  // Each voice's per-block targets. A voice coming on starts clean only when its crossfade has
  // settled at zero; while the crossfade is still up the running voice carries on.
  Targets prepareVoices(const PitchFxParams& p) {
    if (p.autotune.on && !autotuneWas_ && autotuneFade_ == 0.0f) autotune_.reset();
    autotuneWas_ = p.autotune.on;
    const float corr = autotune_.prepare(tracker_.result(), p.autotune);
    bool octaveOn = octave_.prepare(p.octave, corr);
    if (octaveOn && !octaveWas_ && octaveFade_ == 0.0f) {
      octave_.reset();
      octaveOn = octave_.prepare(p.octave, corr);
    }
    octaveWas_ = octaveOn;
    if (p.formant.on && !formantWas_ && formantFade_ == 0.0f) formant_.reset();
    formantWas_ = p.formant.on;
    if (p.formant.tuning.trimDb != formantTrimDb_) {
      formantTrimDb_ = p.formant.tuning.trimDb;
      formantTrim_ = dbToGainFast(formantTrimDb_);
    }
    // The period stays set while the crossfade is up (the tracker runs until it settles), so
    // Autotune's and Formant's grains never lose their pitch mid-fade.
    const bool havePeriod = tracker_.result().period > 0.0f;
    const bool autotuneOn = p.autotune.on && havePeriod;
    const bool formantOn = p.formant.on && havePeriod;
    return {smooth::clamp01(p.octave.mix), octaveOn ? 1.0f : 0.0f, autotuneOn ? 1.0f : 0.0f,
            smooth::clamp01(p.formant.mix), formantOn ? 1.0f : 0.0f};
  }

  bool settledOff(const Targets& t) const {
    return autotuneFade_ == 0.0f && t.autotuneFade == 0.0f && octaveFade_ == 0.0f && t.octaveFade == 0.0f &&
           formantFade_ == 0.0f && t.formantFade == 0.0f;
  }

  // Every stage settled off: the dry gain is exactly 1, so only the ring needs writing. MIX can
  // snap since nothing reads it until a stage comes on.
  void passThrough(const float* in, float* out, int n, const Targets& t) {
    octaveMix_ = t.octaveMix;
    formantMix_ = t.formantMix;
    shiftSet_ = false;
    gains_ = gainsNow();
    for (int i = 0; i < n; ++i) {
      ring_[static_cast<size_t>(head_)] = in[i];
      out[i] = in[i];
      advance();
    }
  }

  // The voice takes Autotune's correction as its pitch ratio (1 with Autotune off), plays its grains
  // at the SHIFT rate and reads at Autotune's voice delay, so the two voices share a sample. The
  // ring runs at most one chunk ahead of the voice's reads, whatever the call's length.
  //
  // SHIFT glides through the 20 ms one-pole, in semitones. A bare step leaves a bump in the grain
  // window sum that reads x1.23 of steady-state curvature next to SHIFT 0, and the glide holds every
  // step under x1.02. It takes the knob outright on the first call after the stage was settled off.
  void prepareFormant(const FormantParams& f, int n) {
    const int st = f.shiftSt < -kFormantMaxShiftSt ? -kFormantMaxShiftSt
                                                    : (f.shiftSt > kFormantMaxShiftSt ? kFormantMaxShiftSt : f.shiftSt);
    if (n != shiftCoefN_) {
      shiftCoef_ = smooth::blockCoef(smooth::kSmoothSec, n);
      shiftCoefN_ = n;
    }
    shiftSm_ = shiftSet_ ? smooth::step(shiftSm_, static_cast<float>(st), shiftCoef_) : static_cast<float>(st);
    shiftSet_ = true;
    const float period = tracker_.result().period;
    formant_.prepare(period, exp2Fast(correctionSemis() * (1.0f / 12.0f)), exp2Fast(shiftSm_ * (1.0f / 12.0f)),
                     autotune_.voiceDelay(period), kBlock);
  }

  // The voice reads a block at a time, so the chunk goes into the ring ahead of the per-sample loop
  // (the loop writes the same samples again), then the voice renders it into formantBuf_.
  void renderFormant(const float* in, int m) {
    int at = head_;
    for (int j = 0; j < m; ++j) {
      ring_[static_cast<size_t>(at)] = in[j];
      at = at + 1 == kVoiceRingLen ? 0 : at + 1;
    }
    formantBuf_.fill(0.0f);
    formant_.render(ring_, head_, m, formantBuf_.data());
  }

  // The smoothers step once per sample; the gains they set are evaluated at the block edges and
  // interpolated between, which is exact once settled. Settled smoothers hold their targets bit for
  // bit, so their steps and the gains are skipped, and a silent Octave voice would only return 0.
  // The Formant voice renders in chunks of kBlock whenever its crossfade is above zero at either end.
  void mixBlock(const float* in, float* out, int n, const Targets& t, const FormantParams& fp) {
    const Gains g0 = gains_;
    const bool formantUp = formantFade_ > 0.0f;
    if (autotuneFade_ != t.autotuneFade || octaveMix_ != t.octaveMix || octaveFade_ != t.octaveFade ||
        formantMix_ != t.formantMix || formantFade_ != t.formantFade) {
      for (int k = 0; k < n; ++k) {
        autotuneFade_ = smooth::step(autotuneFade_, t.autotuneFade, smooth::kSmoothCoef);
        octaveMix_ = smooth::step(octaveMix_, t.octaveMix, smooth::kSmoothCoef);
        octaveFade_ = smooth::step(octaveFade_, t.octaveFade, smooth::kSmoothCoef);
        formantMix_ = smooth::step(formantMix_, t.formantMix, smooth::kSmoothCoef);
        formantFade_ = smooth::step(formantFade_, t.formantFade, smooth::kSmoothCoef);
      }
      gains_ = gainsNow();
    } else if (g0.formantTrim != formantTrim_) {
      gains_ = gainsNow();
    }
    const Gains g1 = gains_;
    const bool formant = formantUp || formantFade_ > 0.0f;
    if (formant) prepareFormant(fp, n);
    const float period = tracker_.result().period;
    const float inv = 1.0f / static_cast<float>(n);
    const bool octave = !octave_.silent();
    for (int c = 0; c < n; c += kBlock) {
      const int m = n - c < kBlock ? n - c : kBlock;
      if (formant) renderFormant(in + c, m);
      for (int i = c; i < c + m; ++i) {
        ring_[static_cast<size_t>(head_)] = in[i];
        const float oct = octave ? octave_.tick(ring_, head_) : 0.0f;
        const float f = static_cast<float>(i + 1) * inv;
        const float corrected = g0.autotune + (g1.autotune - g0.autotune) * f;
        const float dryShare = g0.autotuneDry + (g1.autotuneDry - g0.autotuneDry) * f;
        float base = corrected > 0.0f ? corrected * autotune_.tick(ring_, head_, period) + dryShare * in[i] : in[i];
        if (formant) {
          const float keep = g0.formantBase + (g1.formantBase - g0.formantBase) * f;
          const float shifted = g0.formantWet + (g1.formantWet - g0.formantWet) * f;
          base = keep * base + shifted * formantBuf_[static_cast<size_t>(i - c)];
        }
        const float dry = g0.dry + (g1.dry - g0.dry) * f;
        const float wet = g0.wet + (g1.wet - g0.wet) * f;
        out[i] = dry * base + wet * oct;
        advance();
      }
    }
  }

  // The corrected voice lags the dry input by 23 to 36 ms and may sit a semitone away, so the two
  // add in power. Each crossfade gain is the sine of its own share of the quarter turn, which lands
  // exactly on 0 and 1 at both ends. Formant's MIX is equal-power in the same way, and its toggle
  // fades the mix in linearly, as Octave's does.
  Gains gainsNow() const {
    Gains g;
    g.autotune = sinf(autotuneFade_ * kHalfPi);
    g.autotuneDry = sinf((1.0f - autotuneFade_) * kHalfPi);
    g.dry = 1.0f + (cosf(octaveMix_ * kHalfPi) - 1.0f) * octaveFade_;
    g.wet = sinf(octaveMix_ * kHalfPi) * octaveFade_;
    g.formantBase = 1.0f + (cosf(formantMix_ * kHalfPi) - 1.0f) * formantFade_;
    g.formantWet = sinf(formantMix_ * kHalfPi) * formantFade_ * formantTrim_;
    g.formantTrim = formantTrim_;
    return g;
  }

  // Voices only use head modulo the ring, so wrapping here is exact.
  void advance() { head_ = head_ + 1 == kVoiceRingLen ? 0 : head_ + 1; }

  VoiceRing ring_{};
  PitchTracker tracker_;
  AutotuneVoice autotune_;
  OctaveVoice octave_;
  formant_detail::FormantPsolaVoice formant_;
  std::array<float, kBlock> formantBuf_{};  // the Formant voice's chunk
  float octaveMix_ = 0.0f;
  float octaveFade_ = 0.0f;    // Octave crossfade, 0 off .. 1 on
  float autotuneFade_ = 0.0f;  // Autotune crossfade, 0 dry .. 1 corrected
  float formantMix_ = 0.0f;
  float formantFade_ = 0.0f;   // Formant crossfade, 0 off .. 1 on
  float formantTrimDb_ = std::numeric_limits<float>::quiet_NaN();  // the tuning formantTrim_ came from
  float formantTrim_ = 1.0f;
  float shiftSm_ = 0.0f;       // SHIFT in semitones after the glide
  float shiftCoef_ = 1.0f;     // the glide's coefficient for calls of shiftCoefN_ samples
  int shiftCoefN_ = 0;
  bool shiftSet_ = false;      // shiftSm_ holds a live value
  Gains gains_;               // gains at the end of the last block
  int head_ = 0;
  bool autotuneWas_ = false;  // last block's on-state per card, for enable edges
  bool octaveWas_ = false;
  bool formantWas_ = false;
  bool fresh_ = true;
};

}  // namespace cv
