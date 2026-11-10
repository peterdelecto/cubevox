#pragma once

#include <array>
#include <cstring>

#include "engine/common.h"
#include "engine/fastmath.h"
#include "engine/input_ring.h"
#include "engine/octave.h"
#include "engine/smooth.h"

// The Octave card's engine. It keeps a ring of its own input, cuts the granular voice from it and mixes
// the voice against the input at equal power. The voice needs no pitch, so the engine runs no tracker.
// The toggle's fade and the MIX knob share the one crossfade, so the dry and wet stay equal-power at every
// fade. The card comes on only once its ring holds the taps' deepest read since the ring's last clear, so a
// card started from rest passes its input until then. The voice resets on its enable edge only once the
// crossfade has settled at zero, so a quick off and on keeps the running voice. Settled off, the output is
// the input bit for bit.

namespace cv {

// The ring holds the voice's deepest read, the 80 ms window and the taps' reach (octave.h checks it),
// a block and a pad.
constexpr int kOctaveRingLen = 3920;
static_assert(kOctaveRingLen >= static_cast<int>(OctaveVoice::kMaxWindowMs * kSmpPerMs) + 4 + 3 + kBlock + 4,
              "the ring holds the voice's deepest read");

class OctaveFx {
 public:
  OctaveFx() { reset(); }

  // Leaves the ring zero. A card that rested since it last ran (see rest) has cleared most of the ring
  // already, and this clears what is left.
  void reset() {
    ring_.clear();
    voice_.reset();
    mix_ = 0.0f;
    fade_ = 0.0f;
    gains_ = Gains{};
    was_ = false;
    fresh_ = true;
    rested_ = false;
  }

  // A call for each block the card sits stopped (the chain does not run it); see AutotuneFx::rest.
  void rest() {
    ring_.rest();
    rested_ = true;
  }

  // Runs n samples of any call length, in chunks of at most kBlock. in and out may be the same buffer.
  // A chunk ends where the ring comes to hold the taps' deepest read, so the card comes on at that sample
  // whatever the call length.
  void process(const float* in, float* out, int n, const OctaveParams& p) {
    if (rested_) reset();
    const int deepest = OctaveVoice::deepestRead(p.tuning);
    for (int c = 0; c < n;) {
      int m = n - c < kBlock ? n - c : kBlock;
      const int filled = ring_.filled();
      if (filled < deepest && filled + m > deepest) m = deepest - filled;
      chunk(in + c, out + c, m, p, deepest);
      c += m;
    }
  }

  // True when the voice carries at least half the output's power: the wet gain's sine of its share of
  // the quarter turn reaches 1 / sqrt(2) at a share of 0.5.
  bool leads() const { return mix_ * fade_ >= 0.5f; }

  // The interval the voice plays at in semitones, the SEMITONES step after its glide.
  float intervalSemis() const { return voice_.intervalSemis(); }

  // The delay of the card's output behind its input in samples, the mean over the voices heard. The dry
  // input has none, and the voice lags by half its grain window. The wet share follows the crossfade's
  // power.
  float outputLatency() const { return gains_.wet * gains_.wet * voice_.latency(); }

  // Test hooks: the ring, and the floats the ring clears have zeroed.
  const InputRing<kOctaveRingLen>& ringForTest() const { return ring_; }
  uint32_t ringClearedForTest() const { return ring_.clearedForTest(); }

 private:
  // The equal-power pair: the voice's share and the dry input's. A card that is off contributes exactly
  // 0 and 1.
  struct Gains {
    float wet = 0.0f;
    float dry = 1.0f;
  };

  Gains gainsNow() const {
    Gains g;
    quarterTurn(mix_ * fade_, g.wet, g.dry);
    return g;
  }

  // `deepest` is the taps' deepest read behind the sample they play.
  void chunk(const float* in, float* out, int n, const OctaveParams& p, int deepest) {
    // The card waits until the ring holds what the taps read: `deepest` samples ahead of the chunk's first.
    // A voice coming on starts clean only when its crossfade has settled at zero; while the crossfade is
    // still up the running voice carries on.
    const bool held = ring_.filled() >= deepest;
    const int first = ring_.write(in, n);
    bool on = voice_.prepare(p) && held;
    if (on && !was_ && fade_ == 0.0f) {
      voice_.reset();
      on = voice_.prepare(p);
    }
    was_ = on;
    const float mixTarget = smooth::clamp01(p.mix);
    const float fadeTarget = on ? 1.0f : 0.0f;
    if (fresh_) {
      mix_ = mixTarget;
      fade_ = fadeTarget;
      gains_ = gainsNow();
      fresh_ = false;
    }
    if (fade_ == 0.0f && fadeTarget == 0.0f) {
      // MIX can snap since nothing reads it until the card comes on. The voice keeps its blocker and glide
      // after its fade reaches 0, and clearing them leaves it idle.
      mix_ = mixTarget;
      if (!voice_.idle()) voice_.reset();
      gains_ = Gains{};
      if (out != in) std::memmove(out, in, sizeof(float) * static_cast<size_t>(n));
      return;
    }
    const Gains g0 = gains_;
    if (mix_ != mixTarget || fade_ != fadeTarget) {
      const float a = coef_.at(n);
      mix_ = smooth::step(mix_, mixTarget, a);
      fade_ = smooth::step(fade_, fadeTarget, a);
      gains_ = gainsNow();
    }
    const Gains g1 = gains_;

    // A voice nobody hears does not render: one whose wet gain is 0 at both edges parks.
    const bool heard = (fade_ > 0.0f || fadeTarget > 0.0f) && (g0.wet != 0.0f || g1.wet != 0.0f);
    if (heard) voice_.render(ring_.data(), first, n, buf_.data());
    else if (!voice_.idle()) voice_.park();
    const bool settled = g0.dry == g1.dry && g0.wet == g1.wet;
    if (heard) {
      if (settled) mixLoop<true, true>(in, out, n, g0, g1);
      else mixLoop<true, false>(in, out, n, g0, g1);
    } else {
      if (settled) mixLoop<false, true>(in, out, n, g0, g1);
      else mixLoop<false, false>(in, out, n, g0, g1);
    }
  }

  // The gains ramp by increments; a settled gain holds one value, so it takes no step.
  template <bool Heard, bool Settled>
  void mixLoop(const float* in, float* out, int n, const Gains& g0, const Gains& g1) {
    const float inv = 1.0f / static_cast<float>(n);
    const float dDry = (g1.dry - g0.dry) * inv;
    const float dWet = (g1.wet - g0.wet) * inv;
    float dry = g0.dry;
    float wet = g0.wet;
    for (int j = 0; j < n; ++j) {
      if (!Settled) dry += dDry;
      float y = in[j];
      y *= dry;
      if (Heard) {
        if (!Settled) wet += dWet;
        y += wet * buf_[static_cast<size_t>(j)];
      }
      out[j] = y;
    }
  }

  InputRing<kOctaveRingLen> ring_;
  OctaveVoiceT<kOctaveRingLen> voice_;
  std::array<float, kBlock> buf_{};  // the voice's chunk
  float mix_ = 0.0f;                 // MIX after its smoothing
  float fade_ = 0.0f;                // the toggle's fade, 0 off .. 1 on
  Gains gains_;                      // the gains at the end of the last chunk
  smooth::BlockCoef coef_;           // the 20 ms one-pole's step over the last chunk length
  bool was_ = false;     // the voice was on last chunk
  bool fresh_ = true;    // the next chunk takes its targets outright
  bool rested_ = false;  // the card sat stopped since it last ran
};

}  // namespace cv
