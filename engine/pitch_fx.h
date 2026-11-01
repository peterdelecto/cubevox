#pragma once

#include <cmath>

#include "engine/autotune.h"
#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/octave.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// Shared pitch front end. The tracker hears only the dry input. Autotune, when
// on, crossfades from the dry voice to the corrected one once the tracker has a
// period, and Octave adds Autotune's correction to its interval. The crossfade and
// Octave's MIX are equal-power. A voice resets on its enable edge only once its
// crossfade has settled at zero, so a quick off and on keeps the running voice
// instead of cutting to fresh grains. The tracker's frame is warm only while the
// stage runs. The chain resets the stage when every pitch card is off, so the first
// voiced frame after such a switch-on comes about 32 ms in, where a running stage
// gives it in 8 ms.

namespace cv {

struct PitchFxParams {
  AutotuneParams autotune;
  OctaveParams octave;
  bool crowdOn = false;  // the Crowd card reads the ring and the tracker, so both run
};

class PitchFx {
  static_assert(PitchTracker::kReplaySpan < kVoiceRingLen, "the ring holds a tracker replay's history");

 public:
  PitchFx() { reset(); }

  void reset() {
    ring_.fill(0.0f);
    head_ = 0;
    tracker_.reset();
    autotune_.reset();
    octave_.reset();
    octaveMix_ = 0.0f;
    octaveFade_ = 0.0f;
    autotuneFade_ = 0.0f;
    gains_ = Gains{};
    autotuneWas_ = false;
    octaveWas_ = false;
    fresh_ = true;
  }

  void process(const float* in, float* out, int n, const PitchFxParams& p) {
    // The tracker runs while Autotune is on or still fading, so the voice keeps its period through
    // the crossfade, and while the Crowd card is on. Octave shifts without a pitch, so with neither
    // reader the tracker only keeps hop time, and its next start replays its history from the ring.
    // The 48 kHz fine pass runs only for Autotune; the Crowd card's copies drift by more than the
    // coarse lag misses.
    const bool autotune = p.autotune.on || autotuneFade_ > 0.0f;
    if (autotune || p.crowdOn) {
      tracker_.push(in, n, autotune, PitchTracker::History{ring_.data(), kVoiceRingLen, head_});
    } else {
      tracker_.idle(n);
    }
    const Targets t = prepareVoices(p);
    if (fresh_) {
      octaveMix_ = t.octaveMix;
      octaveFade_ = t.octaveFade;
      autotuneFade_ = t.autotuneFade;
      gains_ = gainsNow();
      fresh_ = false;
    }
    if (settledOff(t)) {
      passThrough(in, out, n, t.octaveMix);
      return;
    }
    mixBlock(in, out, n, t);
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

 private:
  // Where the three smoothers head this block.
  struct Targets {
    float octaveMix;       // Octave MIX
    float octaveFade;      // Octave on, 0 or 1
    float autotuneFade;    // Autotune on with a period, 0 or 1
  };

  // Equal-power gains at the smoothers' current values: Octave's dry and wet, and the crossfade from
  // the dry input to Autotune's corrected voice. A stage that is off contributes exactly 1 and 0.
  struct Gains {
    float dry = 1.0f;
    float wet = 0.0f;
    float autotune = 0.0f;     // the corrected voice's share of the crossfade
    float autotuneDry = 1.0f;  // the dry input's share
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
    // The period stays set while the crossfade is up (the tracker runs until it settles), so
    // Autotune's grains never lose their pitch mid-fade.
    const bool autotuneOn = p.autotune.on && tracker_.result().period > 0.0f;
    return {smooth::clamp01(p.octave.mix), octaveOn ? 1.0f : 0.0f, autotuneOn ? 1.0f : 0.0f};
  }

  bool settledOff(const Targets& t) const {
    return autotuneFade_ == 0.0f && t.autotuneFade == 0.0f && octaveFade_ == 0.0f && t.octaveFade == 0.0f;
  }

  // Every stage settled off: the dry gain is exactly 1, so only the ring needs writing. MIX can
  // snap since nothing reads it until a stage comes on.
  void passThrough(const float* in, float* out, int n, float mixTarget) {
    octaveMix_ = mixTarget;
    gains_ = gainsNow();
    for (int i = 0; i < n; ++i) {
      ring_[static_cast<size_t>(head_)] = in[i];
      out[i] = in[i];
      advance();
    }
  }

  // The smoothers step once per sample; the gains they set are evaluated at the block edges and
  // interpolated between, which is exact once settled. Settled smoothers hold their targets bit for
  // bit, so their steps and the gains are skipped, and a silent Octave voice would only return 0.
  void mixBlock(const float* in, float* out, int n, const Targets& t) {
    const Gains g0 = gains_;
    if (autotuneFade_ != t.autotuneFade || octaveMix_ != t.octaveMix || octaveFade_ != t.octaveFade) {
      for (int k = 0; k < n; ++k) {
        autotuneFade_ = smooth::step(autotuneFade_, t.autotuneFade, smooth::kSmoothCoef);
        octaveMix_ = smooth::step(octaveMix_, t.octaveMix, smooth::kSmoothCoef);
        octaveFade_ = smooth::step(octaveFade_, t.octaveFade, smooth::kSmoothCoef);
      }
      gains_ = gainsNow();
    }
    const Gains g1 = gains_;
    const float period = tracker_.result().period;
    const float inv = 1.0f / static_cast<float>(n);
    const bool octave = !octave_.silent();
    for (int i = 0; i < n; ++i) {
      ring_[static_cast<size_t>(head_)] = in[i];
      const float oct = octave ? octave_.tick(ring_, head_) : 0.0f;
      const float f = static_cast<float>(i + 1) * inv;
      const float corrected = g0.autotune + (g1.autotune - g0.autotune) * f;
      const float dryShare = g0.autotuneDry + (g1.autotuneDry - g0.autotuneDry) * f;
      const float base = corrected > 0.0f ? corrected * autotune_.tick(ring_, head_, period) + dryShare * in[i] : in[i];
      const float dry = g0.dry + (g1.dry - g0.dry) * f;
      const float wet = g0.wet + (g1.wet - g0.wet) * f;
      out[i] = dry * base + wet * oct;
      advance();
    }
  }

  // The corrected voice lags the dry input by 23 to 36 ms and may sit a semitone away, so the two
  // add in power. Each crossfade gain is the sine of its own share of the quarter turn, which lands
  // exactly on 0 and 1 at both ends.
  Gains gainsNow() const {
    Gains g;
    g.autotune = sinf(autotuneFade_ * kHalfPi);
    g.autotuneDry = sinf((1.0f - autotuneFade_) * kHalfPi);
    g.dry = 1.0f + (cosf(octaveMix_ * kHalfPi) - 1.0f) * octaveFade_;
    g.wet = sinf(octaveMix_ * kHalfPi) * octaveFade_;
    return g;
  }

  // Voices only use head modulo the ring, so wrapping here is exact.
  void advance() { head_ = head_ + 1 == kVoiceRingLen ? 0 : head_ + 1; }

  VoiceRing ring_{};
  PitchTracker tracker_;
  AutotuneVoice autotune_;
  OctaveVoice octave_;
  float octaveMix_ = 0.0f;
  float octaveFade_ = 0.0f;    // Octave crossfade, 0 off .. 1 on
  float autotuneFade_ = 0.0f;  // Autotune crossfade, 0 dry .. 1 corrected
  Gains gains_;                // gains at the end of the last block
  int head_ = 0;
  bool autotuneWas_ = false;  // last block's on-state per card, for enable edges
  bool octaveWas_ = false;
  bool fresh_ = true;
};

}  // namespace cv
