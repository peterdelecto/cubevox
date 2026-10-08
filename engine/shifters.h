#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// Pitch shifters shared by Octave and Harmony. Each takes its targets (semitones,
// formant, gain) from the caller once per block and smooths them per sample.
// EpochShifter is option B (epoch PSOLA + formant); GrainShifter is option C
// (asynchronous granular, no tracker).

namespace cv {

// Option B and C settings, with the defaults both stages start from.
struct ShifterTuning {
  float grainPeriods = 2.0f;     // B grain length in sung periods, 1.5..3
  float epochSearch = 0.25f;     // B peak search half-width, fraction of a period
  float epochLpHz = 1000.0f;     // B low-pass used to find the peak
  float grainWindowMs = 40.0f;   // C window, 20..80
  int grainCount = 2;            // C taps, 2 or 4
};

// Pitch-synchronous overlap-add with each grain centred on the glottal peak of
// its period, and each grain read at the formant ratio so vowel size is
// independent of pitch.
class EpochPsolaVoice {
 public:
  static constexpr int kGrainDelay = PsolaVoice::kGrainDelay;
  static constexpr int kMaxGrains = 12;  // 3 periods x ratio 2 / formant 0.5

  void reset() {
    grains_.fill(Grain{});
    markDelay_ = static_cast<float>(kGrainDelay);
    countdown_ = 0.0f;
    lastMark_ = 0.0f;
    lastEpoch_ = 0.0f;
    haveEpoch_ = false;
  }

  // One output sample. ring holds the input, lp the same input low-passed for
  // the peak search; writeCount indexes the sample just written to both.
  // ratio is the pitch ratio, formant the grain playback rate.
  float tick(const VoiceRing& ring, const VoiceRing& lp, long writeCount, float period,
             float ratio, float formant, float grainPeriods, float search) {
    if (period <= 0.0f) return 0.0f;
    const float p = clampf(period, kMinPeriod, kMaxPeriod);
    const float r = clampf(ratio, kMinRatio, kMaxRatio);
    const float outPeriod = p / r;
    const int head = static_cast<int>(writeCount % kVoiceRingLen);

    advanceMarks(p);
    if (countdown_ > outPeriod) countdown_ = outPeriod;
    countdown_ -= 1.0f;
    if (countdown_ <= 0.0f) {
      const float f = clampf(formant, kMinFormant, kMaxFormant);
      const float gp = clampf(grainPeriods, kMinGrainPeriods, kMaxGrainPeriods);
      const float s = clampf(search, 0.0f, kMaxSearch);
      launch(lp, head, p, f, gp * p, s * p, -countdown_);
      countdown_ += outPeriod;
    }

    float sum = 0.0f;
    // Hann grains overlap grainPeriods * ratio / (2 * formant) deep; each
    // grain's gain cancels the grain terms, and 1 / ratio the rest.
    for (Grain& g : grains_) sum += play(ring, head, g);
    return sum / r;
  }

 private:
  struct Grain {
    float delay = 0.0f;  // read point behind the write head; moves by 1 - rate
    float rate = 1.0f;   // input samples per output sample
    float gain = 1.0f;   // overlap compensation for grain length and rate
    float age = 0.0f;
    float dur = 0.0f;    // output samples
    bool active = false;
  };

  static constexpr float kMinPeriod = 48.0f;
  static constexpr float kMaxPeriod = 686.0f;
  static constexpr float kMinRatio = 0.5f;
  static constexpr float kMaxRatio = 2.0f;
  static constexpr float kMinFormant = 0.5f;
  static constexpr float kMaxFormant = 2.0f;
  static constexpr float kMinGrainPeriods = 1.5f;
  static constexpr float kMaxGrainPeriods = 3.0f;
  static constexpr float kMaxSearch = 0.3f;
  static constexpr float kMinSpacing = 0.75f;
  static constexpr float kMinDelay = 3.0f;
  static constexpr float kMaxDelay = kVoiceRingLen - 4.0f;
  static constexpr float kTwoPi = 6.28318530717958647692f;

  // A grain spans len input samples centred on its epoch, so it starts len/2
  // behind the epoch and reaches len * (1 - 1/f) toward the head. That reach
  // stays within len/2 for f <= 2, so the grain never passes its epoch.
  static constexpr float kMaxEpochOffset = (0.5f + kMaxSearch) * kMaxPeriod;
  static constexpr float kMaxLen = kMaxGrainPeriods * kMaxPeriod;
  static_assert(kMaxFormant <= 2.0f, "grain would read past its epoch toward the head");
  static_assert(kGrainDelay - kMaxEpochOffset - 2.0f * kMaxFormant >= kMinDelay,
                "grains would cross the write head");
  static_assert(kGrainDelay + kMaxEpochOffset + kMaxLen * (0.5f + 1.0f / kMinFormant - 1.0f) <=
                    kMaxDelay,
                "slow grains would outrun the ring");

  static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

  // Grid marks as delays behind the write head, as in PsolaVoice. lastMark_
  // and lastEpoch_ age with the head so they compare directly.
  void advanceMarks(float p) {
    markDelay_ += 1.0f;
    while (markDelay_ - p >= kGrainDelay) markDelay_ -= p;
    lastMark_ += 1.0f;
    lastEpoch_ += 1.0f;
  }

  // lead is how far past the ideal launch instant this tick already is.
  void launch(const VoiceRing& lp, int head, float p, float f, float len, float window,
              float lead) {
    float centre = markDelay_;
    if (centre - kGrainDelay > 0.5f * p) centre -= p;

    // A repeat of the same grid mark (pitch up) reuses its epoch.
    float epoch = lastEpoch_;
    if (!haveEpoch_ || fabsf(centre - lastMark_) >= 0.5f * p) {
      const float oldest = haveEpoch_ ? lastEpoch_ - kMinSpacing * p : centre + window;
      epoch = findEpoch(lp, head, centre, window, oldest);
    }
    lastMark_ = centre;
    lastEpoch_ = epoch;
    haveEpoch_ = true;

    Grain* slot = &grains_[0];
    for (Grain& g : grains_) {
      if (!g.active) {
        slot = &g;
        break;
      }
      if (g.age > slot->age) slot = &g;
    }
    const float dur = len / f;
    const float drift = (1.0f - f) * dur;
    const float lo = kMinDelay + (drift < 0.0f ? -drift : 0.0f);
    const float hi = kMaxDelay - (drift > 0.0f ? drift : 0.0f);
    slot->delay = clampf(epoch + 0.5f * len - f * lead, lo, hi);
    slot->rate = f;
    slot->gain = f * 2.0f / (len / p);
    slot->age = lead;
    slot->dur = dur;
    slot->active = true;
  }

  // Delay of the largest low-passed sample within centre +- window and no
  // older than oldest. Falls back to the grid mark if the range is empty.
  static float findEpoch(const VoiceRing& lp, int head, float centre, float window,
                         float oldest) {
    const int lo = static_cast<int>(ceilf(centre - window));
    const float hiF = centre + window < oldest ? centre + window : oldest;
    const int hi = static_cast<int>(floorf(hiF));
    if (hi < lo) return centre;
    int best = lo;
    float bestV = lp[wrap(head - lo)];
    for (int d = lo + 1; d <= hi; ++d) {
      const float v = lp[wrap(head - d)];
      if (v > bestV) {
        bestV = v;
        best = d;
      }
    }
    // Parabola through the peak and its neighbours places the epoch between
    // samples, so grains track a fractional period without jitter.
    if (best <= lo || best >= hi) return static_cast<float>(best);
    const float ym = lp[wrap(head - best + 1)];
    const float yp = lp[wrap(head - best - 1)];
    const float den = ym - 2.0f * bestV + yp;
    return den < 0.0f ? static_cast<float>(best) + 0.5f * (ym - yp) / den
                      : static_cast<float>(best);
  }

  float play(const VoiceRing& ring, int head, Grain& g) const {
    if (!g.active) return 0.0f;
    if (g.age >= g.dur) {
      g.active = false;
      return 0.0f;
    }
    const float w = 0.5f * (1.0f - cosf(kTwoPi * g.age / g.dur));
    const float x = readCubic(ring, head, g.delay);
    g.age += 1.0f;
    g.delay += 1.0f - g.rate;
    return g.gain * w * x;
  }

  static int wrap(int i) {
    i %= kVoiceRingLen;
    return i < 0 ? i + kVoiceRingLen : i;
  }

  // Catmull-Rom at a fractional delay behind the newest sample.
  static float readCubic(const VoiceRing& ring, int head, float delay) {
    const float pos = static_cast<float>(head) - delay;
    const float fl = floorf(pos);
    const float t = pos - fl;
    const int i0 = wrap(static_cast<int>(fl));
    const float xm = ring[wrap(i0 - 1)];
    const float x0 = ring[i0];
    const float x1 = ring[wrap(i0 + 1)];
    const float x2 = ring[wrap(i0 + 2)];
    const float c1 = 0.5f * (x1 - xm);
    const float c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return x0 + t * (c1 + t * (c2 + t * c3));
  }

  std::array<Grain, kMaxGrains> grains_{};
  float markDelay_ = static_cast<float>(kGrainDelay);
  float countdown_ = 0.0f;
  float lastMark_ = 0.0f;
  float lastEpoch_ = 0.0f;
  bool haveEpoch_ = false;
};

// Option B with gain, glide and formant smoothing around the voice.
class EpochShifter {
 public:
  EpochShifter() { reset(); }

  void reset() {
    voice_.reset();
    semis_ = 0.0f;
    formant_ = 0.0f;
    gain_ = 0.0f;
    semisT_ = 0.0f;
    formantT_ = 0.0f;
    gainT_ = 0.0f;
    aGlide_ = 1.0f;
    grainPeriods_ = ShifterTuning{}.grainPeriods;
    search_ = ShifterTuning{}.epochSearch;
    fresh_ = true;
  }

  // Called once per block. semis and formant in semitones, gain linear.
  void prepare(float semis, float formant, float gain, float glideMs, float grainPeriods,
               float search) {
    semisT_ = semis;
    formantT_ = formant;
    gainT_ = gain;
    // Smoothers start on target after reset; a silent voice jumps to its new
    // interval instead of gliding in from a stale one.
    if (fresh_ || gain_ == 0.0f) {
      semis_ = semisT_;
      formant_ = formantT_;
    }
    if (fresh_) gain_ = gainT_;
    fresh_ = false;
    aGlide_ = smooth::coef(glideMs * 0.001f);
    grainPeriods_ = grainPeriods;
    search_ = search;
  }

  // One sample of the voice, gain, glide and formant applied.
  float tick(const VoiceRing& ring, const VoiceRing& lp, long writeCount, float period) {
    gain_ = smooth::step(gain_, gainT_, smooth::kSmoothCoef);
    semis_ = smooth::step(semis_, semisT_, aGlide_);
    formant_ = smooth::step(formant_, formantT_, aGlide_);
    if (gain_ == 0.0f) return 0.0f;
    return gain_ * voice_.tick(ring, lp, writeCount, period, exp2f(semis_ / 12.0f),
                               exp2f(formant_ / 12.0f), grainPeriods_, search_);
  }

 private:
  EpochPsolaVoice voice_;
  float semis_ = 0.0f;
  float formant_ = 0.0f;
  float gain_ = 0.0f;
  float semisT_ = 0.0f;
  float formantT_ = 0.0f;
  float gainT_ = 0.0f;
  float aGlide_ = 1.0f;
  float grainPeriods_ = 2.0f;
  float search_ = 0.25f;
  bool fresh_ = true;
};

// Option C: N taps ride a shared sawtooth over a fixed window, Hann-crossfaded,
// so no tracker is involved and formants move with the pitch.
class GrainShifter {
 public:
  static constexpr int kMaxGrains = 4;

  GrainShifter() { reset(); }

  void reset() {
    phase_ = 0.0f;
    semis_ = 0.0f;
    gain_ = 0.0f;
    semisT_ = 0.0f;
    gainT_ = 0.0f;
    aGlide_ = 1.0f;
    ratio_ = 1.0f;
    ratioSemis_ = 0.0f;
    windowSmp_ = ShifterTuning{}.grainWindowMs * kSmpPerMs;
    grains_ = ShifterTuning{}.grainCount;
    fresh_ = true;
  }

  // Called once per block. semis in semitones, gain linear.
  void prepare(float semis, float gain, float glideMs, float windowMs, int grainCount) {
    semisT_ = semis;
    gainT_ = gain;
    // A silent voice jumps to its new interval instead of gliding in.
    if (fresh_ || gain_ == 0.0f) semis_ = semisT_;
    if (fresh_) gain_ = gainT_;
    fresh_ = false;
    aGlide_ = smooth::coef(glideMs * 0.001f);
    windowSmp_ = clampf(windowMs, kMinWindowMs, kMaxWindowMs) * kSmpPerMs;
    grains_ = grainCount >= 4 ? 4 : 2;
    // Each grain's Hann rotor restarts from the exact phase every block; the
    // step uses this block's ratio, so a glide is off by a hair until the
    // next block re-syncs it.
    if (semis_ != ratioSemis_) {
      ratio_ = exp2f(semis_ / 12.0f);
      ratioSemis_ = semis_;
    }
    const float inv = 1.0f / static_cast<float>(grains_);
    const float step = kTwoPi * (1.0f - ratio_) / windowSmp_;
    for (int k = 0; k < grains_; ++k)
      window_[k].init(kTwoPi * frac(phase_ + static_cast<float>(k) * inv), step);
  }

  // One sample of the voice, gain and glide applied.
  float tick(const VoiceRing& ring, long writeCount) {
    gain_ = smooth::step(gain_, gainT_, smooth::kSmoothCoef);
    semis_ = smooth::step(semis_, semisT_, aGlide_);
    if (gain_ == 0.0f) return 0.0f;

    const int head = static_cast<int>(writeCount);
    if (semis_ != ratioSemis_) {
      ratio_ = exp2f(semis_ / 12.0f);
      ratioSemis_ = semis_;
    }
    const float ratio = ratio_;
    const float d0 = 0.5f * windowSmp_ + kMinDelay;
    const float inv = 1.0f / static_cast<float>(grains_);

    // A tap at ratio r drifts (1 - r) samples of delay per sample; the
    // sawtooth is that drift normalised to the window.
    float sum = 0.0f;
    for (int k = 0; k < grains_; ++k) {
      const float p = frac(phase_ + static_cast<float>(k) * inv);
      const float g = window_[k].window();
      window_[k].advance();
      sum += g * readCubic(ring, head, d0 + windowSmp_ * (p - 0.5f));
    }
    phase_ = frac(phase_ + (1.0f - ratio) / windowSmp_);
    return gain_ * sum * 2.0f * inv;
  }

 private:
  static constexpr float kSmpPerMs = kSampleRate / 1000.0f;
  static constexpr float kMinWindowMs = 20.0f;
  static constexpr float kMaxWindowMs = 80.0f;
  static constexpr float kMinDelay = 4.0f;
  static constexpr float kTwoPi = 6.28318530717958647692f;
  static_assert(kMaxWindowMs * kSmpPerMs + kMinDelay + 3.0f < kVoiceRingLen,
                "grains would outrun the ring");

  static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
  static float frac(float x) {
    x -= floorf(x);
    return x < 0.0f ? x + 1.0f : x;
  }

  // Reads sit within one ring length of the head, so one step wraps them; the
  // final clamp is never taken and lets the compiler bound the index.
  static int wrap(int i) {
    if (i < 0) i += kVoiceRingLen;
    else if (i >= kVoiceRingLen) i -= kVoiceRingLen;
    return static_cast<unsigned>(i) < static_cast<unsigned>(kVoiceRingLen) ? i : 0;
  }

  // Catmull-Rom at a fractional delay behind the newest sample.
  static float readCubic(const VoiceRing& ring, int head, float delay) {
    const float pos = static_cast<float>(head) - delay;
    const float fl = floorf(pos);
    const float t = pos - fl;
    const int i0 = wrap(static_cast<int>(fl));
    const float xm = ring[wrap(i0 - 1)];
    const float x0 = ring[i0];
    const float x1 = ring[wrap(i0 + 1)];
    const float x2 = ring[wrap(i0 + 2)];
    const float c1 = 0.5f * (x1 - xm);
    const float c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return x0 + t * (c1 + t * (c2 + t * c3));
  }

  float phase_ = 0.0f;
  float semis_ = 0.0f;
  float gain_ = 0.0f;
  float semisT_ = 0.0f;
  float gainT_ = 0.0f;
  float aGlide_ = 1.0f;
  float ratio_ = 1.0f;       // exp2 of ratioSemis_ / 12, recomputed only while gliding
  float ratioSemis_ = 0.0f;
  float windowSmp_ = 1920.0f;
  int grains_ = 2;
  std::array<HannRotor, kMaxGrains> window_{};
  bool fresh_ = true;
};

}  // namespace cv
