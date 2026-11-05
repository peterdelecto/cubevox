#pragma once

#include <array>
#include <cmath>
#include <cstring>
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
// same amount as Autotune's, so the two line up: they take the same grain grid
// whichever starts second. It mixes with the base (the dry voice, or Autotune's
// crossfade) ahead of Octave. Autotune's crossfade and Octave's MIX are
// equal-power. Formant's MIX uses linear gains and a level gain that follows how
// far its two voices correlate (see levelGainAt). A voice resets on its enable
// edge only once its crossfade has settled at zero, so a quick off and on keeps
// the running voice instead of cutting to fresh grains. The tracker's frame is
// warm only while the stage runs. The chain resets the stage when every pitch
// card is off, so the first voiced frame after such a switch-on comes about 32 ms
// in, where a running stage gives it in 8 ms. At a note that starts after a rest, the
// period the voices read moves to the new note from the tracker's onset estimate, ahead
// of its first voiced frame (pitch.h). Autotune's correction still moves only on voiced
// frames.

namespace cv {

// The panel's SHIFT range, whole semitones, and the SHIFT and MIX the card starts on.
constexpr int kFormantMaxShiftSt = 7;
constexpr int kFormantShiftSteps = 2 * kFormantMaxShiftSt + 1;
constexpr int kFormantDefaultShiftSt = -3;
constexpr float kFormantDefaultMix = 1.0f;

// The voice's limit, whole semitones either way: the stage clamps the SHIFT it is handed to this, and
// the grains then play at 0.5 to 2 times, the voice's rate range. Yawn's RANGE reaches -12 st.
constexpr int kFormantVoiceMaxShiftSt = 12;

// The averaging time of the running powers behind the Formant mix's level gain, and that gain's
// ceiling (+6 dB).
constexpr float kFormantPowerSec = 0.04f;
constexpr float kFormantLevelGainMax = 2.0f;
constexpr float kFormantPowerFloor = 1e-20f;  // below this a power is silence

// The shifted voice's trim, which the engine clamps to this many dB either way.
constexpr float kFormantMaxTrimDb = 24.0f;

// How far the corrected voice and the shifted voice correlate at a SHIFT of 0, 1, 2 and 3 st or
// more, measured on Adam's three clips with Autotune on. The level gain starts from it when the
// corrected voice has sat out at MIX 100 % and comes back.
constexpr std::array<float, 4> kFormantCoherence = {0.95f, 0.4f, 0.1f, 0.0f};

struct FormantTuning {
  // The shifted voice's trim in dB at each SHIFT from -7 to +7 st, fit on Adam's three clips with
  // Autotune off so that the mean of the voice alone at MIX 100 % lands on +0.25 dB, the middle of 0
  // to +0.5. The grains sum to a level that follows SHIFT, and one trim cannot hold it.
  std::array<float, kFormantShiftSteps> trimDb = {4.29f, 3.87f, 3.12f, 2.36f, 1.98f, 1.72f, 1.01f, 0.20f,
                                                  0.94f, 1.44f, 1.51f, 1.77f, 1.87f, 1.60f, 1.64f};
  float offsetDb = 0.0f;  // added to every entry, for the render tool

  bool operator==(const FormantTuning& o) const { return trimDb == o.trimDb && offsetDb == o.offsetDb; }
};

struct FormantParams {
  bool on = false;
  int shiftSt = kFormantDefaultShiftSt;  // knob A (SHIFT) -7..+7 st, Yawn -12..+6 st; the grains play at 2^(shiftSt / 12)
  float mix = kFormantDefaultMix;        // knob B (MIX), 0..1
  FormantTuning tuning;
};

struct PitchFxParams {
  AutotuneParams autotune;
  OctaveParams octave;
  FormantParams formant;
  bool crowdOn = false;  // the Mob card reads the ring and the tracker, so both run
  bool growlOn = false;  // the Growl card reads them too
};

class PitchFx {
  static_assert(PitchTracker::kReplaySpan < kVoiceRingLen, "the ring holds a tracker replay's history");

 public:
  // The floats of the ring one rest() call zeroes: the ring is clear after kVoiceRingLen / this calls,
  // 25 blocks.
  static constexpr int kRestClearFloats = 512;

  PitchFx() {
    formant_.latchGain(true);
    reset();
  }

  // Leaves the ring zero. A stage that rested since it last ran (see rest) has cleared most of it already,
  // and this clears what is left.
  void reset() {
    clearRing(kVoiceRingLen);
    zeroRun_ = kVoiceRingLen;
    head_ = 0;
    tracker_.reset();
    autotune_.reset();
    octave_.reset();
    formant_.reset();
    resetLevelGain();
    octaveMix_ = 0.0f;
    octaveFade_ = 0.0f;
    autotuneFade_ = 0.0f;
    formantMix_ = 0.0f;
    formantFade_ = 0.0f;
    shiftSet_ = false;
    gains_ = Gains{};
    autotuneWas_ = false;
    autotuneIdle_ = false;
    octaveWas_ = false;
    formantWas_ = false;
    formantStart_ = false;
    rangeBad_ = false;
    fresh_ = true;
  }

  void process(const float* in, float* out, int n, const PitchFxParams& p) {
    // The tracker runs while Autotune or Formant is on or still fading, so the voice keeps its period
    // through the crossfade, and while the Mob or Growl card is on. Octave shifts without a pitch, so with no
    // reader the tracker only keeps hop time, and its next start replays its history from the ring.
    // The 48 kHz fine pass runs for Autotune and Formant, whose voices must sit on the sung pitch to
    // the cent; the Mob card's copies drift by more than the coarse lag misses.
    const bool autotune = p.autotune.on || autotuneFade_ > 0.0f;
    const bool formant = formantWanted(p.formant) || formantFade_ > 0.0f;
    writeCall(in, n);
    tracker_.readOutOfRange(p.crowdOn || formantWanted(p.formant));
    if (autotune || formant || p.crowdOn || p.growlOn) {
      tracker_.push(in, n, true, autotune || formant, PitchTracker::History{ring_.data(), kVoiceRingLen, head_});
    } else {
      tracker_.idle(n);
    }
    const Targets t = prepareVoices(p, n);
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

  // A call for each block the stage sits stopped (the chain does not run it). It zeroes the next
  // kRestClearFloats floats of the input ring, so the reset that starts the stage again finds the ring
  // clear and the first block after a switch-on carries none of the 51 kB clear. A stage that restarts
  // before the ring is clear pays for the rest in that reset.
  void rest() { clearRing(kRestClearFloats); }

  // The floats the ring clears have zeroed since construction, for the tests. It wraps.
  uint32_t ringClearedForTest() const { return ringCleared_; }

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

  // The Formant crossfade, 0 off .. 1 on, and SHIFT after its glide in semitones, for the tests.
  float formantFade() const { return formantFade_; }
  float shiftSemis() const { return shiftSm_; }

 private:
  // Where the smoothers head this block.
  struct Targets {
    float octaveMix;       // Octave MIX
    float octaveFade;      // Octave on, 0 or 1
    float autotuneFade;    // Autotune on with a period, 0 or 1
    float formantMix;      // Formant MIX
    float formantFade;     // Formant on with a period, 0 or 1
  };

  // Gains at the smoothers' current values: Octave's dry and wet and the crossfade from the dry
  // input to Autotune's corrected voice (equal-power), and Formant's share of the base and of the
  // shifted voice (linear). A stage that is off contributes exactly 1 and 0.
  struct Gains {
    float dry = 1.0f;
    float wet = 0.0f;
    float autotune = 0.0f;     // the corrected voice's share of the crossfade
    float autotuneDry = 1.0f;  // the dry input's share
    float formantBase = 1.0f;  // the base's share after Formant's mix
    float formantWet = 0.0f;   // the shifted voice's share, trim included
    float formantTrim = 1.0f;  // the trim formantWet carries
  };

  // The sums of base * base, voice * voice and base * voice over a call, for the running powers.
  struct Sums {
    float bb = 0.0f;
    float vv = 0.0f;
    float bv = 0.0f;
  };

  // What one block's samples are mixed with: the gains at its two edges, the level gain at its two
  // edges, and which voices run.
  struct MixBlock {
    Gains g0;
    Gains g1;
    float levelGain0 = 1.0f;
    float levelGain1 = 1.0f;
    float inv = 0.0f;     // 1 / n
    float period = 0.0f;
    float sumBB = 0.0f;   // over the block, base * base, voice * voice and base * voice, for the powers
    float sumVV = 0.0f;
    float sumBV = 0.0f;
    bool formant = false;
    bool octave = false;        // the Octave voice runs and its wet gain is above 0
    bool skipAutotune = false;  // Formant at MIX 100 % leaves the base unheard
    bool baseUnheard = false;   // the corrected voice was skipped, so the base was not rendered
    bool settled = false;       // every gain holds the same value at both edges
  };

  static bool formantWanted(const FormantParams& f) { return f.on && f.mix > 0.0f; }

  // Each voice's per-block targets. A voice coming on starts clean only when its crossfade has
  // settled at zero; while the crossfade is still up the running voice carries on.
  Targets prepareVoices(const PitchFxParams& p, int n) {
    if (p.autotune.on && !autotuneWas_ && autotuneFade_ == 0.0f) {
      autotune_.reset();
      autotuneIdle_ = false;
      if (formantFade_ > 0.0f) autotune_.adoptGrid(formant_.grid());
    }
    autotuneWas_ = p.autotune.on;
    const PitchResult& pr = tracker_.result();
    const float corr = autotune_.prepare(pr, p.autotune, n);
    bool octaveOn = octave_.prepare(p.octave, corr);
    if (octaveOn && !octaveWas_ && octaveFade_ == 0.0f) {
      octave_.reset();
      octaveOn = octave_.prepare(p.octave, corr);
    }
    octaveWas_ = octaveOn;
    const bool wanted = formantWanted(p.formant);
    if (wanted && !formantWas_ && formantFade_ == 0.0f) {
      formant_.reset();
      resetLevelGain();
      shiftSet_ = false;
      formantStart_ = true;
    }
    formantWas_ = wanted;
    // A voiced frame on the edge of the tracker's range, or past it, reads the edge, not the note, so
    // Formant steps aside for the dry voice until a frame inside the range returns. Unvoiced frames
    // hold the state, as they hold the period.
    if (pr.voiced) rangeBad_ = pr.clamped || pr.outOfRange;
    // The period stays set while the crossfade is up (the tracker runs until it settles), so
    // Autotune's and Formant's grains never lose their pitch mid-fade.
    const bool havePeriod = pr.period > 0.0f;
    const bool autotuneOn = p.autotune.on && havePeriod;
    const bool formantOn = wanted && havePeriod && !rangeBad_;
    return {smooth::clamp01(p.octave.mix), octaveOn ? 1.0f : 0.0f, autotuneOn ? 1.0f : 0.0f,
            smooth::clamp01(p.formant.mix), formantOn ? 1.0f : 0.0f};
  }

  bool settledOff(const Targets& t) const {
    return autotuneFade_ == 0.0f && t.autotuneFade == 0.0f && octaveFade_ == 0.0f && t.octaveFade == 0.0f &&
           formantFade_ == 0.0f && t.formantFade == 0.0f;
  }

  // The ring takes the whole call before the tracker or a voice reads it: the tracker's fine pass
  // reads its frame from the ring, and the voices read only samples older than the one they render
  // (Autotune's grains sit over 1,000 samples back, Octave's under 3,850), so none reads the
  // call's later samples, and the call overwrites only the oldest ones. head_ stays at the call's
  // first sample, which the voices step over.
  void writeCall(const float* in, int n) {
    ringZero_ = 0;
    int at = head_;
    for (int done = 0; done < n;) {
      const int run = n - done < kVoiceRingLen - at ? n - done : kVoiceRingLen - at;
      std::memcpy(&ring_[static_cast<size_t>(at)], in + done, sizeof(float) * static_cast<size_t>(run));
      at = at + run == kVoiceRingLen ? 0 : at + run;
      done += run;
    }
  }

  // Every stage settled off: the dry gain is exactly 1, and the ring already holds the call. MIX can
  // snap since nothing reads it until a stage comes on.
  void passThrough(const float* in, float* out, int n, const Targets& t) {
    octaveMix_ = t.octaveMix;
    formantMix_ = t.formantMix;
    shiftSet_ = false;
    // The Octave voice keeps its blocker and glide after its fade reaches 0. Clearing them here leaves
    // the voice idle.
    if (!octave_.idle()) octave_.reset();
    gains_ = Gains{};
    gains_.formantTrim = formantTrim_;
    if (out != in) std::memmove(out, in, sizeof(float) * static_cast<size_t>(n));
    head_ = (head_ + n) % kVoiceRingLen;
    noteZeros(in, n);
  }

  // The voice takes Autotune's correction as its pitch ratio (1 with Autotune off), plays its grains
  // at the SHIFT rate and reads at Autotune's voice delay, so the two voices share a sample. The
  // ring holds the whole call before the voice reads, whatever the call's length.
  //
  // SHIFT glides through the 20 ms one-pole, in semitones. A bare step leaves a bump in the grain
  // window sum that reads x1.23 of steady-state curvature next to SHIFT 0, and the glide holds every
  // step under x1.02. It takes the knob outright on the first call after the voice starts. The
  // playback rate and the trim follow the glided SHIFT and are worked out when it moves.
  //
  // The voice holds one ratio for the call, the correction at the call's middle. The call that lays
  // the marks reads it at the call's start, where the first grain launches. A voice that starts
  // beside a running Autotune takes its grain grid.
  void prepareFormant(const FormantParams& f, int n) {
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
      formantTrim_ = dbToGainFast(clampf(trimDbAt(shiftSm_), -kFormantMaxTrimDb, kFormantMaxTrimDb));
      trimSemis_ = shiftSm_;
    }
    const float period = tracker_.result().period;
    const float corr = correctionAfter(formant_.starting() ? 0.0f : 0.5f * static_cast<float>(n));
    formant_.prepare(period, corr == 0.0f ? 1.0f : exp2Fast(corr * (1.0f / 12.0f)), shiftRate_,
                     autotune_.voiceDelay(period), kBlock);
    if (formantStart_) {
      formantStart_ = false;
      if (autotuneFade_ > 0.0f && !autotuneIdle_) formant_.adoptGrid(autotune_.grid());
    }
  }

  // The trim in dB at the glided SHIFT, read between the two entries around it. Past -7 or +7 st it
  // holds the end entry.
  float trimDbAt(float st) const {
    const float pos = clampf(st + static_cast<float>(kFormantMaxShiftSt), 0.0f, static_cast<float>(kFormantShiftSteps - 1));
    const int i = static_cast<int>(pos);
    const int j = i + 1 < kFormantShiftSteps ? i + 1 : i;
    const float a = tuning_.trimDb[static_cast<size_t>(i)];
    const float b = tuning_.trimDb[static_cast<size_t>(j)];
    return a + (b - a) * (pos - static_cast<float>(i)) + tuning_.offsetDb;
  }

  // Autotune's correction k samples into the call, scaled as correctionSemis() scales it.
  float correctionAfter(float k) const {
    if (!autotuneWas_ && autotuneFade_ == 0.0f) return 0.0f;
    const float live = autotune_.correctionAfter(k);
    return autotuneWas_ ? live : live * autotuneFade_;
  }

  // The Octave and Formant voices render a block at a time, in chunks of at most kBlock samples. The
  // chunk's samples are in the ring already (writeCall), and zeroBefore_ takes the run of exact zeros
  // that ends just ahead of the chunk.
  void startChunk(const float* in, int m) {
    zeroBefore_ = zeroRun_;
    noteZeros(in, m);
  }

  // How many samples at the newest end of the ring are exactly 0, after m more samples in[].
  // The count of a voiced chunk ends at its last sample.
  void noteZeros(const float* in, int m) {
    int run = 0;
    while (run < m && in[m - 1 - run] == 0.0f) ++run;
    zeroRun_ = run < m ? run : (zeroRun_ + m < kVoiceRingLen ? zeroRun_ + m : kVoiceRingLen);
  }

  // The Formant voice renders the chunk now in the ring into formantBuf_. A grain that reads only
  // the zeros before the chunk adds nothing, so the voice skips its taps.
  void renderFormant(int m) {
    formantBuf_.fill(0.0f);
    formant_.render(ring_, head_, m, formantBuf_.data(), zeroBefore_);
  }

  // The 20 ms one-pole's coefficient for one step over n samples, kept for the last n.
  float smoothCoefFor(int n) {
    if (n != coefN_) {
      coef_ = smooth::blockCoef(smooth::kSmoothSec, n);
      coefN_ = n;
    }
    return coef_;
  }

  // Each smoother steps once per block, and the gains it sets are evaluated at the block edges and
  // interpolated between. Settled smoothers hold their targets bit for bit, so their steps and the
  // gains are skipped.
  void stepSmoothers(int n, const Targets& t) {
    if (autotuneFade_ != t.autotuneFade || octaveMix_ != t.octaveMix || octaveFade_ != t.octaveFade ||
        formantMix_ != t.formantMix || formantFade_ != t.formantFade) {
      if (n > 0) {
        const float a = smoothCoefFor(n);
        autotuneFade_ = smooth::step(autotuneFade_, t.autotuneFade, a);
        octaveMix_ = smooth::step(octaveMix_, t.octaveMix, a);
        octaveFade_ = smooth::step(octaveFade_, t.octaveFade, a);
        formantMix_ = smooth::step(formantMix_, t.formantMix, a);
        formantFade_ = smooth::step(formantFade_, t.formantFade, a);
      }
      gains_ = gainsNow();
    }
  }

  // The mix of one call. With Formant up at either end the shifted voice renders in chunks of kBlock
  // and the powers behind the level gain follow; otherwise the plain loops run.
  void mixBlock(const float* in, float* out, int n, const Targets& t, const FormantParams& fp) {
    MixBlock m = beginBlock(n, t, fp);
    if (m.formant) {
      if (m.settled) formantLoop<true>(in, out, n, m);
      else formantLoop<false>(in, out, n, m);
      absorbPowers(m, n);
    } else {
      mixPlain(in, out, n, m);
    }
  }

  // Steps the smoothers and settles what the call runs. A voice nobody hears does not render: an
  // Octave voice whose wet gain is 0 at both edges parks, and at MIX 100 % the base is multiplied by 0,
  // so Autotune's voice sits out and restarts clean when it is heard again.
  MixBlock beginBlock(int n, const Targets& t, const FormantParams& fp) {
    MixBlock m;
    m.g0 = gains_;
    const bool formantUp = formantFade_ > 0.0f;
    stepSmoothers(n, t);
    m.g1 = gains_;
    m.formant = formantUp || formantFade_ > 0.0f;
    if (m.formant) {
      prepareFormant(fp, n);
      if (formantTrim_ != m.g1.formantTrim) {
        m.g1.formantTrim = formantTrim_;
        m.g1.formantWet = formantMix_ * formantFade_ * formantTrim_;
        gains_ = m.g1;
      }
    }
    m.levelGain0 = levelGain_;
    if (m.formant) clearIdlePowers();
    m.levelGain1 = m.formant ? levelGainAt(formantMix_ * formantFade_) : 1.0f;
    levelGain_ = m.levelGain1;
    m.inv = 1.0f / static_cast<float>(n);
    m.period = tracker_.result().period;
    m.octave = (octaveFade_ > 0.0f || t.octaveFade > 0.0f) && (m.g0.wet != 0.0f || m.g1.wet != 0.0f);
    if (!m.octave && !octave_.idle()) octave_.park();
    m.skipAutotune = m.formant && m.g0.formantBase == 0.0f && m.g1.formantBase == 0.0f;
    if (!m.skipAutotune && autotuneIdle_) {
      autotune_.restartVoice();
      if (m.formant) autotune_.adoptGrid(formant_.grid());
      autotuneIdle_ = false;
    }
    m.baseUnheard = m.skipAutotune && (m.g0.autotune > 0.0f || m.g1.autotune > 0.0f);
    if (m.baseUnheard) autotuneIdle_ = true;
    m.settled = settledGains(m);
    return m;
  }

  // True when no gain moves over the call, so each holds one value for every sample.
  static bool settledGains(const MixBlock& m) {
    return m.g0.dry == m.g1.dry && m.g0.wet == m.g1.wet && m.g0.autotune == m.g1.autotune &&
           m.g0.autotuneDry == m.g1.autotuneDry && m.g0.formantBase == m.g1.formantBase &&
           m.g0.formantWet == m.g1.formantWet && m.levelGain0 == m.levelGain1;
  }

  // The gain at fraction f of the call; a settled gain is the same at both edges.
  template <bool Settled>
  static float gainAt(float a, float b, float f) {
    return Settled ? b : a + (b - a) * f;
  }

  // The mix with Formant up. Each output sample is the gains' interpolation at its place in the call,
  // or the gains themselves when they hold still.
  template <bool Settled>
  void formantLoop(const float* in, float* CV_RESTRICT out, int n, MixBlock& m) {
    Sums sums;
    for (int c = 0; c < n; c += kBlock) {
      const int len = n - c < kBlock ? n - c : kBlock;
      startChunk(in + c, len);
      if (m.octave) octave_.render(ring_, head_, len, octBuf_.data());
      renderFormant(len);
      renderAutotune(len, m);
      for (int j = 0; j < len; ++j) {
        const int i = c + j;
        const size_t k = static_cast<size_t>(j);
        const float oct = m.octave ? octBuf_[k] : 0.0f;
        const float f = Settled ? 0.0f : static_cast<float>(i + 1) * m.inv;
        out[i] = formantSample<Settled>(m, sums, in[i], voiceBuf_[k], formantBuf_[k], oct, f);
      }
      advance(len);
    }
    m.sumBB = sums.bb;
    m.sumVV = sums.vv;
    m.sumBV = sums.bv;
  }

  // The corrected voice for a chunk, into voiceBuf_, when anyone hears it. A base the mix leaves
  // unheard only has its correction step along.
  void renderAutotune(int len, const MixBlock& m) {
    if (m.g0.autotune <= 0.0f && m.g1.autotune <= 0.0f) return;
    if (m.skipAutotune) autotune_.glide(len);
    else autotune_.render(ring_, head_, len, m.period, voiceBuf_.data());
  }

  // One output sample at fraction f of the call, from the dry input, the corrected voice's sample av,
  // the Formant voice's sample and the Octave voice's. The base and voice powers add into sums.
  template <bool Settled>
  float formantSample(const MixBlock& m, Sums& sums, float in, float av, float voice, float oct, float f) {
    const float corrected = gainAt<Settled>(m.g0.autotune, m.g1.autotune, f);
    float base = in;
    if (corrected > 0.0f) {
      if (m.skipAutotune) {
        base = 0.0f;
      } else {
        const float dryShare = gainAt<Settled>(m.g0.autotuneDry, m.g1.autotuneDry, f);
        base = corrected * av + dryShare * in;
      }
    }
    const float keep = gainAt<Settled>(m.g0.formantBase, m.g1.formantBase, f);
    const float shifted = gainAt<Settled>(m.g0.formantWet, m.g1.formantWet, f);
    sums.bb += base * base;
    sums.vv += voice * voice;
    sums.bv += base * voice;
    base = (keep * base + shifted * voice) * gainAt<Settled>(m.levelGain0, m.levelGain1, f);
    const float dry = gainAt<Settled>(m.g0.dry, m.g1.dry, f);
    const float wet = gainAt<Settled>(m.g0.wet, m.g1.wet, f);
    return dry * base + wet * oct;
  }

  // The correlation of the two voices at a glided SHIFT, read between the entries around it.
  static float coherenceAt(float st) {
    const float pos = clampf(st < 0.0f ? -st : st, 0.0f, static_cast<float>(kFormantCoherence.size() - 1));
    const size_t i = static_cast<size_t>(pos);
    const float a = kFormantCoherence[i];
    const float b = kFormantCoherence[i + 1 < kFormantCoherence.size() ? i + 1 : i];
    return a + (b - a) * (pos - static_cast<float>(i));
  }

  // The block's sums move the three running powers one step of the one-pole. An unheard base stands
  // in at the shifted voice's level and the correlation that SHIFT gives, so the level gain is near
  // right the moment MIX leaves 100 %.
  void absorbPowers(const MixBlock& m, int n) {
    if (n <= 0) return;
    if (n != powerCoefN_) {
      powerCoef_ = smooth::blockCoef(kFormantPowerSec, n);
      powerCoefN_ = n;
    }
    voiceSq_ += powerCoef_ * (m.sumVV * m.inv - voiceSq_);
    if (m.baseUnheard) {
      baseSq_ = voiceSq_;
      baseVoice_ = coherenceAt(shiftSm_) * voiceSq_;
    } else {
      baseSq_ += powerCoef_ * (m.sumBB * m.inv - baseSq_);
      baseVoice_ += powerCoef_ * (m.sumBV * m.inv - baseVoice_);
    }
  }

  // The mix with no Formant, in the loop that fits the voices running and whether the gains move.
  void mixPlain(const float* in, float* out, int n, const MixBlock& m) {
    const bool autotune = m.g0.autotune > 0.0f || m.g1.autotune > 0.0f;
    if (m.octave) {
      if (autotune) plainLoopFor<true, true>(in, out, n, m);
      else plainLoopFor<true, false>(in, out, n, m);
    } else {
      if (autotune) plainLoopFor<false, true>(in, out, n, m);
      else plainLoopFor<false, false>(in, out, n, m);
    }
  }

  template <bool Octave, bool Autotune>
  void plainLoopFor(const float* in, float* out, int n, const MixBlock& m) {
    if (m.settled) plainLoop<Octave, Autotune, true>(in, out, n, m);
    else plainLoop<Octave, Autotune, false>(in, out, n, m);
  }

  // The gains ramp by increments; a settled gain holds one value, so it takes no step.
  template <bool Octave, bool Autotune, bool Settled>
  void plainLoop(const float* in, float* out, int n, const MixBlock& m) {
    const float dDry = (m.g1.dry - m.g0.dry) * m.inv;
    const float dWet = (m.g1.wet - m.g0.wet) * m.inv;
    const float dCor = (m.g1.autotune - m.g0.autotune) * m.inv;
    const float dDsh = (m.g1.autotuneDry - m.g0.autotuneDry) * m.inv;
    for (int c = 0; c < n; c += kBlock) {
      const int len = n - c < kBlock ? n - c : kBlock;
      startChunk(in + c, len);
      if (Octave) octave_.render(ring_, head_, len, octBuf_.data());
      if (Autotune) autotune_.render(ring_, head_, len, m.period, voiceBuf_.data());
      const float at = static_cast<float>(c) * m.inv;  // the chunk's place in the call
      float dry = m.g0.dry + (m.g1.dry - m.g0.dry) * at;
      float wet = m.g0.wet + (m.g1.wet - m.g0.wet) * at;
      float cor = m.g0.autotune + (m.g1.autotune - m.g0.autotune) * at;
      float dsh = m.g0.autotuneDry + (m.g1.autotuneDry - m.g0.autotuneDry) * at;
      for (int j = 0; j < len; ++j) {
        if (!Settled) dry += dDry;
        float y = in[c + j];
        if (Autotune) {
          if (!Settled) {
            cor += dCor;
            dsh += dDsh;
          }
          if (cor > 0.0f) y = cor * voiceBuf_[static_cast<size_t>(j)] + dsh * y;
        }
        y *= dry;
        if (Octave) {
          if (!Settled) wet += dWet;
          y += wet * octBuf_[static_cast<size_t>(j)];
        }
        out[c + j] = y;
      }
      advance(len);
    }
  }

  // sin and cos of a share of the quarter turn, share in [0, 1]. They land exactly on 0 and 1 at both
  // ends, which a series would miss by a hair.
  static void quarterTurn(float share, float& s, float& c) {
    if (share <= 0.0f) {
      s = 0.0f;
      c = 1.0f;
    } else if (share >= 1.0f) {
      s = 1.0f;
      c = 0.0f;
    } else {
      sinCosFast(share * kHalfPi, s, c);
    }
  }

  // The corrected voice lags the dry input by 23 to 36 ms and may sit a semitone away, so the two
  // add in power. Each crossfade gain is the sine or cosine of its own share of the quarter turn.
  // Octave's turn is its MIX scaled by the toggle's fade, so the dry and wet stay equal-power at every
  // fade. Formant's gains are mix and 1 - mix, scaled by the toggle's fade, and levelGainAt() corrects
  // the level they leave.
  Gains gainsNow() const {
    Gains g;
    quarterTurn(autotuneFade_, g.autotune, g.autotuneDry);
    quarterTurn(octaveMix_ * octaveFade_, g.wet, g.dry);
    g.formantBase = 1.0f - formantMix_ * formantFade_;
    g.formantWet = formantMix_ * formantFade_ * formantTrim_;
    g.formantTrim = formantTrim_;
    return g;
  }

  // The base and the shifted voice share a pitch, so how they add depends on SHIFT. Near 0 they are
  // almost one waveform and add in amplitude. A few semitones out they are unrelated and add in
  // power. No fixed pair of gains holds the level across both. The running powers of the two voices
  // and of their product give the power the linear gains produce. This returns the gain that raises
  // it to the voices' own powers averaged at weight x, the mix scaled by the toggle's fade. The
  // gain is 1 for identical voices and 1.41 at x = 0.5 for unrelated ones of equal level. It is
  // exactly 1 at x = 0 and 1, so MIX 0 and MIX 100 % pass the plain voices.
  float levelGainAt(float x) const {
    if (x <= 0.0f || x >= 1.0f) return 1.0f;
    const float keep = 1.0f - x;
    const float shifted = x * formantTrim_;
    const float want = keep * baseSq_ + x * formantTrim_ * formantTrim_ * voiceSq_;
    const float got = keep * keep * baseSq_ + shifted * shifted * voiceSq_ + 2.0f * keep * shifted * baseVoice_;
    if (got < kFormantPowerFloor || want < kFormantPowerFloor) return 1.0f;
    const float gain = sqrtf(want / got);
    return gain < 1.0f ? 1.0f : (gain > kFormantLevelGainMax ? kFormantLevelGainMax : gain);
  }

  // The running powers clear once both fall under the floor, which digital silence would otherwise
  // decay into the denormal range, and when a NaN or an infinity has got into them, which no later
  // sample would wash out.
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

  // Zeroes up to `most` more floats of the ring, in index order from ringZero_.
  void clearRing(int most) {
    const int n = most < kVoiceRingLen - ringZero_ ? most : kVoiceRingLen - ringZero_;
    zeroFloats(&ring_[static_cast<size_t>(ringZero_)], static_cast<size_t>(n));
    ringZero_ += n;
    ringCleared_ += static_cast<uint32_t>(n);
  }

  // Steps head_ over m samples, m under the ring's length. Voices only use head modulo the ring, so
  // wrapping here is exact.
  void advance(int m) { head_ = head_ + m < kVoiceRingLen ? head_ + m : head_ + m - kVoiceRingLen; }

  VoiceRing ring_{};
  PitchTracker tracker_;
  AutotuneVoice autotune_;
  OctaveVoice octave_;
  formant_detail::FormantPsolaVoice formant_;

  // The voices' chunks.
  std::array<float, kBlock> formantBuf_{};  // the Formant voice's
  std::array<float, kBlock> octBuf_{};      // the Octave voice's
  std::array<float, kBlock> voiceBuf_{};    // the corrected voice's

  // The smoothers behind the crossfades, 0 off .. 1 on.
  float octaveMix_ = 0.0f;
  float octaveFade_ = 0.0f;    // Octave
  float autotuneFade_ = 0.0f;  // Autotune, 0 dry .. 1 corrected
  float formantMix_ = 0.0f;
  float formantFade_ = 0.0f;   // Formant
  float coef_ = 1.0f;          // their coefficient for steps of coefN_ samples
  int coefN_ = 0;
  Gains gains_;                // the gains at the end of the last block

  // Formant: the tuning, the glided SHIFT and what follows from it, and the level gain's running powers.
  FormantTuning tuning_;
  float shiftSm_ = 0.0f;       // SHIFT in semitones after the glide
  float shiftRate_ = 1.0f;     // the grains' playback rate at shiftSm_
  float formantTrim_ = 1.0f;   // the trim at shiftSm_
  float baseSq_ = 0.0f;        // running power of the base, one step a block
  float voiceSq_ = 0.0f;       // running power of the shifted voice before its trim
  float baseVoice_ = 0.0f;     // running mean of base * voice
  float levelGain_ = 1.0f;     // the level gain at the end of the last block
  float powerCoef_ = 1.0f;     // the powers' coefficient for steps of powerCoefN_ samples
  int powerCoefN_ = 0;
  bool shiftSet_ = false;      // shiftSm_ holds a live value

  // The SHIFT that rateSemis_ and trimSemis_ were worked out at; NaN until the first.
  float rateSemis_ = std::numeric_limits<float>::quiet_NaN();
  float trimSemis_ = std::numeric_limits<float>::quiet_NaN();

  // The ring: where the next sample goes, how many samples at its newest end are exactly 0, and how far
  // from index 0 the ring is known to be zero (a write drops it to 0). ringCleared_ counts the floats
  // the clears have zeroed.
  int head_ = 0;
  int ringZero_ = kVoiceRingLen;
  uint32_t ringCleared_ = 0;
  int zeroRun_ = kVoiceRingLen;
  int zeroBefore_ = 0;  // the same count just before the chunk being rendered

  // Last block's on-state per card, for enable edges, and what the voices wait on.
  bool autotuneWas_ = false;
  bool octaveWas_ = false;
  bool formantWas_ = false;
  bool formantStart_ = false;  // Formant started and its first call has yet to take Autotune's grid
  bool rangeBad_ = false;      // the last voiced frame sat on the edge of the tracker's range or past it
  bool autotuneIdle_ = false;  // Autotune's voice sat out blocks and restarts clean when heard again
  bool fresh_ = true;
};

}  // namespace cv
