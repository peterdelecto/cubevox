#pragma once

#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/formant_params.h"
#include "engine/formant_voice.h"
#include "engine/input_ring.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/smooth.h"
#include "engine/upstream_pitch.h"

// The engine of the Formant card and of Apraxia, which drives the same engine with its MIX pinned at 1.
// It keeps a ring of its own input and cuts a grain voice from it. The voice plays at pitch ratio 1 and
// moves the formants by the SHIFT rate. The base, the other half of MIX, is the same ring read at the
// voice's lag, so the two line up at every setting and at SHIFT 0 the voice rebuilds the base.
//
// The grains sit on a period grid. Directly behind the Autotune card the engine reads Autotune's tracker:
// the chain hands it the tracker's result, the shift Autotune put on the pitch and the delay it put on
// the audio. Anywhere else the engine runs a tracker of its own on its own input, since the input can
// carry two pitches at once. In both cases the voice reads at the tracker's alignment less the delay
// already on the audio, held over the floor a grain centred on a mark needs.
//
// The toggle's fade crosses the card's input to the mixed output over the 20 ms one-pole. The mix has
// linear gains, and a level gain that follows how far the base and the voice correlate (see levelGainAt)
// holds the level across SHIFT. A voice resets on its enable edge only once its crossfade has settled at
// zero, so a quick off and on keeps the running voice. Settled off, the output is the input bit for bit.

namespace cv {

constexpr int kFormantRingLen = formant_detail::kGrainRingLen;
static_assert(PitchTracker::kReplaySpan < kFormantRingLen, "the ring holds a tracker replay's history");

// The crossfade's chunk ramps run through a per-sample one-pole of 5 ms. The base sits a lag behind the
// input, so a ramp that starts with a slope puts a kink in the output of that slope times the difference
// between the two; the one-pole turns each slope change into a smooth bend.
inline const float kFadeSmooth = smooth::coef(0.005f);

class FormantFx {
 public:
  using Voice = formant_detail::FormantPsolaVoiceT<kFormantRingLen>;

  FormantFx() {
    voice_.latchGain(true);
    reset();
  }

  // Leaves the ring zero and the trackers cold. A card that rested since it last ran (see rest) has
  // cleared most of the ring already, and this clears what is left.
  void reset() {
    ring_.clear();
    zeroRun_ = kFormantRingLen;
    tracker_.reset();
    voice_.reset();
    resetLevelGain();
    mix_ = 0.0f;
    fade_ = 0.0f;
    fadeSm_ = 0.0f;
    shiftSet_ = false;
    gains_ = Gains{};
    was_ = false;
    rangeBad_ = false;
    lagSet_ = false;
    lag_ = 0.0f;
    fresh_ = true;
    rested_ = false;
  }

  // A call for each block the card sits stopped (the chain does not run it); see AutotuneFx::rest.
  void rest() {
    ring_.rest();
    rested_ = true;
  }

  // Runs n samples of any call length, in chunks of at most kBlock. in and out may be the same buffer.
  // `up` names Autotune's tracker when the card sits directly behind the Autotune card, and is empty
  // otherwise.
  void process(const float* in, float* out, int n, const FormantParams& p, const UpstreamPitch& up = UpstreamPitch{}) {
    if (rested_) reset();
    for (int c = 0; c < n; c += kBlock) chunk(in + c, out + c, n - c < kBlock ? n - c : kBlock, p, up);
  }

  // The crossfade, 0 off .. 1 on, and SHIFT after its glide in semitones, for the tests and the render
  // tool.
  float fade() const { return fade_; }
  float shiftSemis() const { return shiftSm_; }

  // The delay of the card's output behind its input in samples: the delay the voice and the base play at,
  // weighted by the toggle's fade. The input crosses to the mix as the fade rises.
  float outputLatency() const { return fade_ * baseLag_; }

  // The tracker of the card's own, for the tests.
  const PitchResult& ownPitch() const { return tracker_.result(); }

  // Test hooks: the ring, the floats the ring clears have zeroed, the lag the voice read at last, and the
  // level gain.
  const InputRing<kFormantRingLen>& ringForTest() const { return ring_; }
  uint32_t ringClearedForTest() const { return ring_.clearedForTest(); }
  float lagForTest() const { return lag_; }
  float baseLagForTest() const { return baseLag_; }
  float levelGainForTest() const { return levelGain_; }

 private:
  // The crossfade's fade and the mix's gains: the base's share, and the shifted voice's with its trim.
  struct Gains {
    float fade = 0.0f;
    float base = 1.0f;
    float voice = 0.0f;
    float trim = 1.0f;
  };

  // The sums of base * base, voice * voice and base * voice over a chunk, for the running powers.
  struct Sums {
    float bb = 0.0f;
    float vv = 0.0f;
    float bv = 0.0f;
  };

  static bool wantedBy(const FormantParams& f) { return f.on && f.mix > 0.0f; }

  void chunk(const float* in, float* out, int n, const FormantParams& p, const UpstreamPitch& up) {
    const bool wanted = wantedBy(p);
    const int first = ring_.write(in, n);
    const int zeroBefore = zeroRun_;
    noteZeros(in, n);
    const PitchResult& pr = pitchFor(in, n, first, wanted, up);

    // A voice coming on starts clean only when its crossfade has settled at zero; while the crossfade is
    // still up the running voice carries on.
    if (wanted && !was_ && fade_ == 0.0f) {
      voice_.reset();
      resetLevelGain();
      shiftSet_ = false;
    }
    was_ = wanted;

    // A voiced frame on the edge of the tracker's range, or past it, reads the edge, not the note, so the
    // card steps aside for the dry voice until a frame inside the range returns. Unvoiced frames hold the
    // state, as they hold the period. The period stays set while the crossfade is up, so the grains never
    // lose their pitch mid-fade.
    if (pr.voiced) rangeBad_ = pr.clamped || pr.outOfRange;
    const bool on = wanted && pr.period > 0.0f && !rangeBad_;
    const float mixTarget = smooth::clamp01(p.mix);
    const float fadeTarget = on ? 1.0f : 0.0f;
    if (fresh_) {
      mix_ = mixTarget;
      fade_ = fadeTarget;
      fadeSm_ = fadeTarget;
      gains_ = gainsNow();
      fresh_ = false;
    }
    if (fade_ == 0.0f && fadeTarget == 0.0f && fadeSm_ == 0.0f) {
      mix_ = mixTarget;
      shiftSet_ = false;
      lagSet_ = false;
      gains_ = gainsNow();
      if (out != in) std::memmove(out, in, sizeof(float) * static_cast<size_t>(n));
      return;
    }
    const Gains g0 = gains_;
    if (mix_ != mixTarget || fade_ != fadeTarget) {
      const float a = smoothCoefFor(n);
      mix_ = smooth::step(mix_, mixTarget, a);
      fade_ = smooth::step(fade_, fadeTarget, a);
    }
    prepareVoice(p, n, pr, up);
    gains_ = gainsNow();
    const Gains g1 = gains_;
    clearIdlePowers();
    const float level0 = levelGain_;
    levelGain_ = smooth::step(level0, levelGainAt(mix_), smoothCoefFor(n));
    mix(in, out, n, first, zeroBefore, g0, g1, level0);
  }

  // The tracker result the chunk reads: Autotune's when the chain hands it over, else the card's own,
  // which runs while the card is on or fading. The 48 kHz fine pass runs, since the grains must sit on the
  // sung pitch.
  const PitchResult& pitchFor(const float* in, int n, int first, bool wanted, const UpstreamPitch& up) {
    if (up.result != nullptr) return *up.result;
    tracker_.readOutOfRange(wanted);
    if (wanted || fade_ > 0.0f) {
      tracker_.push(in, n, true, true, PitchTracker::History{ring_.data().data(), kFormantRingLen, first});
    } else {
      tracker_.idle(n);
    }
    return tracker_.result();
  }

  // SHIFT glides through the 20 ms one-pole, in semitones. A bare step leaves a bump in the grain window
  // sum that reads x1.23 of steady-state curvature next to SHIFT 0, and the glide holds every step under
  // x1.02. It takes the knob outright on the first call after the voice starts. The playback rate and the
  // trim follow the glided SHIFT and are worked out when it moves.
  //
  // The voice plays at pitch ratio 1 on the period the card's input carries: the tracker's, divided by the
  // ratio of the shift Autotune put on it. It reads at the tracker's alignment, the grain delay plus a
  // period, less the delay already on the audio. The base's delay slides toward the voice's at
  // kDryLagSlew a sample and starts on it.
  void prepareVoice(const FormantParams& f, int n, const PitchResult& pr, const UpstreamPitch& up) {
    const int st = f.shiftSt < -kFormantVoiceMaxShiftSt
                       ? -kFormantVoiceMaxShiftSt
                       : (f.shiftSt > kFormantVoiceMaxShiftSt ? kFormantVoiceMaxShiftSt : f.shiftSt);
    const float target = static_cast<float>(st);
    if (!shiftSet_) {
      shiftSm_ = target;
      shiftSet_ = true;
    } else if (shiftSm_ != target) {
      shiftSm_ = smooth::step(shiftSm_, target, smoothCoefFor(n));
    }
    if (shiftSm_ != rateSemis_) {
      shiftRate_ = exp2Fast(shiftSm_ * (1.0f / 12.0f));
      rateSemis_ = shiftSm_;
    }
    if (!(f.tuning == tuning_)) {
      tuning_ = f.tuning;
      trimSemis_ = std::numeric_limits<float>::quiet_NaN();
    }
    if (shiftSm_ != trimSemis_) {
      trim_ = dbToGainFast(clampf(trimDbAt(shiftSm_), -kFormantMaxTrimDb, kFormantMaxTrimDb));
      trimSemis_ = shiftSm_;
    }
    const float shift = up.result != nullptr ? up.shiftSemis : 0.0f;
    const float delayed = up.result != nullptr ? up.latency : 0.0f;
    const float period = shift == 0.0f ? pr.period : pr.period / exp2Fast(shift * (1.0f / 12.0f));
    const float aligned = static_cast<float>(PsolaVoice::kGrainDelay) + PitchTracker::clampPeriod(pr.period) - delayed;
    const float lag = formant_detail::clampLag<kFormantRingLen>(aligned, period);
    voice_.prepare(period, 1.0f, shiftRate_, lag, kBlock);
    lag_ = lag;
    followVoice(PitchTracker::clampPeriod(period), voice_.outputLag(), n);
  }

  // The base reads where the voice actually plays: the grain grid keeps the phase of its history, so the
  // voice's delay sits up to half a period from its lag. The base takes whole periods in one step, which a
  // periodic input hides, and slides the rest at kDryLagSlew a sample, so it never jumps by a fraction.
  void followVoice(float period, float delay, int n) {
    if (!lagSet_) {
      baseLag0_ = baseLag_ = delay;
      lagSet_ = true;
      return;
    }
    baseLag_ += floorf((delay - baseLag_) / period + 0.5f) * period;
    baseLag0_ = baseLag_;
    baseLag_ = smooth::toward(baseLag_, delay, formant_detail::kDryLagSlew * static_cast<float>(n));
  }

  // The trim in dB at the glided SHIFT, read between the two entries around it. Past -7 or +7 st it holds
  // the end entry.
  float trimDbAt(float st) const {
    const float pos = clampf(st + static_cast<float>(kFormantMaxShiftSt), 0.0f, static_cast<float>(kFormantShiftSteps - 1));
    const int i = static_cast<int>(pos);
    const int j = i + 1 < kFormantShiftSteps ? i + 1 : i;
    const float a = tuning_.trimDb[static_cast<size_t>(i)];
    const float b = tuning_.trimDb[static_cast<size_t>(j)];
    return a + (b - a) * (pos - static_cast<float>(i)) + tuning_.offsetDb;
  }

  // The mix of one chunk. The voice renders from the ring. The base is the ring read at the sliding lag,
  // and is skipped when MIX sits at 100 % at both edges: the powers then take a stand-in at the
  // correlation SHIFT gives.
  void mix(const float* in, float* out, int n, int first, int zeroBefore, const Gains& g0, const Gains& g1,
           float level0) {
    voiceBuf_.fill(0.0f);
    voice_.render(ring_.data(), first, n, voiceBuf_.data(), zeroBefore);
    const bool needBase = g0.base != 0.0f || g1.base != 0.0f;
    const float inv = 1.0f / static_cast<float>(n);
    if (needBase) readBase(first, n, inv);
    const float dFade = (g1.fade - g0.fade) * inv;
    const float dBase = (g1.base - g0.base) * inv;
    const float dVoice = (g1.voice * g1.trim - g0.voice * g0.trim) * inv;
    const float dLevel = (levelGain_ - level0) * inv;
    float fade = g0.fade;
    float keep = g0.base;
    float wet = g0.voice * g0.trim;
    float level = level0;
    float fadeSm = fadeSm_;
    Sums sums;
    for (int j = 0; j < n; ++j) {
      fade += dFade;
      fadeSm += kFadeSmooth * (fade - fadeSm);
      if (fabsf(fade - fadeSm) < smooth::kSnap) fadeSm = fade;
      keep += dBase;
      wet += dVoice;
      level += dLevel;
      const size_t k = static_cast<size_t>(j);
      const float voice = voiceBuf_[k];
      const float base = needBase ? baseBuf_[k] : 0.0f;
      sums.bb += base * base;
      sums.vv += voice * voice;
      sums.bv += base * voice;
      const float mixed = (keep * base + wet * voice) * level;
      out[j] = in[j] + fadeSm * (mixed - in[j]);
    }
    fadeSm_ = fadeSm;
    absorbPowers(sums, inv, needBase, n);
  }

  // The base for the chunk into baseBuf_: the ring read at the sliding lag.
  void readBase(int first, int n, float inv) {
    const float lagStep = (baseLag_ - baseLag0_) * inv;
    float at = baseLag0_;
    for (int j = 0; j < n; ++j) {
      at += lagStep;
      baseBuf_[static_cast<size_t>(j)] =
          readRing<kFormantRingLen>(ring_.data().data(), wrapOnce<kFormantRingLen>(first + j), at);
    }
  }

  // How many samples at the newest end of the ring are exactly 0, after m more samples in[]. The count of
  // a voiced chunk ends at its last sample. A grain that reads only the zeros before a chunk adds nothing,
  // so the voice skips its taps.
  void noteZeros(const float* in, int m) {
    int run = 0;
    while (run < m && in[m - 1 - run] == 0.0f) ++run;
    zeroRun_ = run < m ? run : (zeroRun_ + m < kFormantRingLen ? zeroRun_ + m : kFormantRingLen);
  }

  Gains gainsNow() const {
    Gains g;
    g.fade = fade_;
    g.base = 1.0f - mix_;
    g.voice = mix_;
    g.trim = trim_;
    return g;
  }

  // The correlation of the base and the shifted voice at a glided SHIFT, read between the entries around it.
  static float coherenceAt(float st) {
    const float pos = clampf(st < 0.0f ? -st : st, 0.0f, static_cast<float>(kFormantCoherence.size() - 1));
    const size_t i = static_cast<size_t>(pos);
    const float a = kFormantCoherence[i];
    const float b = kFormantCoherence[i + 1 < kFormantCoherence.size() ? i + 1 : i];
    return a + (b - a) * (pos - static_cast<float>(i));
  }

  // The chunk's sums move the three running powers one step of the one-pole. An unheard base stands in at
  // the shifted voice's level and the correlation that SHIFT gives, so the level gain is near right the
  // moment MIX leaves 100 %.
  void absorbPowers(const Sums& s, float inv, bool baseHeard, int n) {
    if (n <= 0) return;
    if (n != powerCoefN_) {
      powerCoef_ = smooth::blockCoef(kFormantPowerSec, n);
      powerCoefN_ = n;
    }
    voiceSq_ += powerCoef_ * (s.vv * inv - voiceSq_);
    if (!baseHeard) {
      baseSq_ = voiceSq_;
      baseVoice_ = coherenceAt(shiftSm_) * voiceSq_;
    } else {
      baseSq_ += powerCoef_ * (s.bb * inv - baseSq_);
      baseVoice_ += powerCoef_ * (s.bv * inv - baseVoice_);
    }
  }

  // The base and the shifted voice share a pitch, so how they add depends on SHIFT. Near 0 they are almost
  // one waveform and add in amplitude. A few semitones out they are unrelated and add in power. No fixed
  // pair of gains holds the level across both. The running powers of the two voices and of their product
  // give the power the linear gains produce. This returns the gain that raises it to the voices' own
  // powers averaged at weight x, the MIX. The gain is 1 for identical voices and 1.41 at x = 0.5 for
  // unrelated ones of equal level. It is exactly 1 at x = 0 and 1, so MIX 0 and MIX 100 % pass the plain
  // voices.
  float levelGainAt(float x) const {
    if (x <= 0.0f || x >= 1.0f) return 1.0f;
    const float keep = 1.0f - x;
    const float shifted = x * trim_;
    const float want = keep * baseSq_ + x * trim_ * trim_ * voiceSq_;
    const float got = keep * keep * baseSq_ + shifted * shifted * voiceSq_ + 2.0f * keep * shifted * baseVoice_;
    if (got < kFormantPowerFloor || want < kFormantPowerFloor) return 1.0f;
    const float gain = sqrtf(want / got);
    return gain < 1.0f ? 1.0f : (gain > kFormantLevelGainMax ? kFormantLevelGainMax : gain);
  }

  // The running powers clear once both fall under the floor, which digital silence would otherwise decay
  // into the denormal range, and when a NaN or an infinity has got into them, which no later sample would
  // wash out.
  void clearIdlePowers() {
    const float sum = baseSq_ + voiceSq_;
    const bool bad = !(sum >= 0.0f && sum < std::numeric_limits<float>::infinity());
    if (bad || (baseSq_ < kFormantPowerFloor && voiceSq_ < kFormantPowerFloor)) resetLevelGain();
  }

  // Clears the running powers and the level gain.
  void resetLevelGain() {
    baseSq_ = 0.0f;
    voiceSq_ = 0.0f;
    baseVoice_ = 0.0f;
    levelGain_ = 1.0f;
  }

  // The 20 ms one-pole's coefficient for one step over n samples, kept for the last n.
  float smoothCoefFor(int n) {
    if (n != coefN_) {
      coef_ = smooth::blockCoef(smooth::kSmoothSec, n);
      coefN_ = n;
    }
    return coef_;
  }

  InputRing<kFormantRingLen> ring_;
  PitchTracker tracker_;  // the card's own, for a card that does not sit directly behind Autotune
  Voice voice_;
  std::array<float, kBlock> voiceBuf_{};  // the shifted voice's chunk
  std::array<float, kBlock> baseBuf_{};   // the base's chunk

  float mix_ = 0.0f;   // MIX after its smoothing
  float fade_ = 0.0f;  // the crossfade from the input to the mix, 0 off .. 1 on
  float fadeSm_ = 0.0f;  // the crossfade after the per-sample one-pole that rounds the chunk edges
  Gains gains_;        // the gains at the end of the last chunk
  float coef_ = 1.0f;  // the 20 ms one-pole's coefficient for steps of coefN_ samples
  int coefN_ = 0;

  // SHIFT: the tuning, the glided value in semitones and what follows from it, and the level gain's
  // running powers.
  FormantTuning tuning_;
  float shiftSm_ = 0.0f;
  float shiftRate_ = 1.0f;  // the grains' playback rate at shiftSm_
  float trim_ = 1.0f;       // the trim at shiftSm_
  float baseSq_ = 0.0f;     // running power of the base, one step a chunk
  float voiceSq_ = 0.0f;    // running power of the shifted voice before its trim
  float baseVoice_ = 0.0f;  // running mean of base * voice
  float levelGain_ = 1.0f;  // the level gain at the end of the last chunk
  float powerCoef_ = 1.0f;  // the powers' coefficient for steps of powerCoefN_ samples
  int powerCoefN_ = 0;
  bool shiftSet_ = false;   // shiftSm_ holds a live value

  // The SHIFT that shiftRate_ and trim_ were worked out at; NaN until the first.
  float rateSemis_ = std::numeric_limits<float>::quiet_NaN();
  float trimSemis_ = std::numeric_limits<float>::quiet_NaN();

  float lag_ = 0.0f;      // the delay the voice reads at
  float baseLag_ = 0.0f;   // the delay the base reads at the chunk's end, sliding toward lag_
  float baseLag0_ = 0.0f;  // and at its start
  bool lagSet_ = false;   // baseLag_ holds a live value
  int zeroRun_ = kFormantRingLen;  // samples at the ring's newest end that are exactly 0

  bool was_ = false;       // the card was wanted last chunk
  bool rangeBad_ = false;  // the last voiced frame sat on the edge of the tracker's range or past it
  bool fresh_ = true;      // the next chunk takes its targets outright
  bool rested_ = false;    // the card sat stopped since it last ran
};

}  // namespace cv
