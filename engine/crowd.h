#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/crowd_epoch.h"
#include "engine/crowd_trial.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/pitch_fx.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// The Crowd card adds up to four copies of the voice, each a different person saying the same line
// a little after Adam. Each copy sits a few semitones off the lead, has its own throat (a grain
// playback rate) and its own tone shelf, and lags the lead by its own delay. The crowd as a whole
// has a fixed low cut. The copies never read a key. They read the pitch period and Autotune's
// correction from PitchFx and cut their grains from its input ring.
//
// VOICES fades the copies in one at a time (low, high, low, high), then fades the lead out, so the
// top of the knob is the crowd alone. Total power is unity at every position. WIDTH spreads the
// copies' pitch and formant offsets from none to their full values, and stretches their lags. The
// bottom tenth of the knob is for instruments and melodic lines, where the copies are plain delayed
// doubles of the lead, exactly on its pitch at WIDTH 0. WIDTH also scales the drift and the contour,
// so nothing else moves a copy there. A copy changes its lag only in a quiet gap, so it never bends
// in pitch mid-word. Scatter gives each copy a fresh timing offset at every gap, and drift walks each
// copy's pitch slowly around its offset.
//
// Three more controls make a copy read as a person and not as Adam shifted. Level scatter gives a
// copy a new loudness at every gap. Contour sets how much of Adam's rises and falls a copy follows.
// Epoch align cuts every grain on a glottal pulse instead of on the period grid, which removes the
// buzz and the doubled pulses a grid cut leaves when a copy's throat moves. All three stay inside
// what a human voice does, and the engine clamps each to that range. Each is neutral at 0 (contour
// 1, epoch align off), and the output is then bit for bit the output without it. The defaults are
// the owner's working set.
//
// Voice 5 is a trial copy outside the VOICES law (owner 2026-10-31). It tries to sound like a person of
// another sex, age or body size, with its own pitch, throat, contour, spectral tilt, breath, vocal fry,
// peak, notch, jitter and shimmer. It is off by default, and off it does not tick and moves no sample
// of the other four.

namespace cv {

struct CrowdTuning {
  std::array<float, 4> pitchSt = {-1.75f, 2.75f, -3.0f, 1.0f};     // full pitch offset 2^(st / 12) at WIDTH 1
  std::array<float, 4> formantSt = {-1.75f, -1.0f, -1.0f, -1.5f};  // grain playback rate 2^(st / 12) at WIDTH 1
  std::array<float, 4> tiltDb = {-3.0f, -4.0f, 3.0f, 5.0f};        // high shelf gain above tiltHz
  std::array<float, 4> delayMs = {40.0f, 100.0f, 70.0f, 75.0f};    // lag behind the lead before lagScale
  float lagScale = 1.2f;       // a copy's lag is delayMs times this
  float tiltHz = 2500.0f;
  float voiceDb = -9.0f;       // one voice against the lead at unity
  float trimDb = 1.0f;         // level rule residue
  float leadFadeFrom = 0.8f;   // VOICES position where the lead starts to leave
  float holdFrom = 0.75f;      // part of each voice's fifth spent fading; the rest holds
  float hpHz = 120.0f;         // low cut on the crowd sum
  float maxLagMs = 200.0f;     // ceiling on a copy's lag
  float gapFloorDb = -50.0f;   // a copy's read point under this level counts as a gap
  float scatterMs = 12.0f;     // peak random lag offset a copy takes at each gap
  float driftCents = 15.0f;    // peak of each copy's slow random pitch walk
  float driftHoldS = 2.0f;     // time between new drift targets

  // WIDTH scales a copy's pitch and formant offsets by a factor that is linear between three
  // points. It is 0 at WIDTH 0, widthKneeFrac at WIDTH widthKnee and 1 at WIDTH 1. At WIDTH 0 every
  // copy sits exactly on the lead's pitch with the lead's throat. The same factor scales the drift
  // range and pulls each contour toward 1, so the bottom of the knob stays in tune for a melodic line.
  float widthKnee = 0.1f;
  float widthKneeFrac = 0.012f;

  // WIDTH also stretches every lag by one multiplier that is widthLagBottom at WIDTH 0 (0.2..1) and
  // 1 + widthLagMs / (the longest base lag, delayMs times lagScale) at WIDTH 1, linear between. At
  // WIDTH 1 the longest copy lags widthLagMs (0..60) behind where lagScale alone puts it, and the
  // others in proportion.
  float widthLagBottom = 0.5f;
  float widthLagMs = 20.0f;

  // Each copy takes a new loudness at every gap, uniform in +- this many dB (0..4).
  float levelScatterDb = 2.0f;

  // The share of Adam's pitch deviation a copy follows (0.6..1.4). 1 follows him exactly, 0.7 flattens
  // the copy's rises and falls to 70 % of his, 1.3 exaggerates them.
  std::array<float, 4> contour = {0.8f, 1.2f, 0.7f, 1.1f};

  // True cuts each grain on the glottal pulse nearest its grid mark, except in a copy whose pitch
  // ratio and grain playback rate are both exactly 1 (WIDTH 0), which stays on the grid. epochSearch
  // is the half-width in periods of the peak search around each expected pulse (0..0.3).
  bool epochAlign = true;
  float epochSearch = 0.25f;

  // Voice 5. It adds to the crowd at voice5Db against the lead whatever VOICES says, and the lead
  // law counts its power with the others. Its lag follows the same law from delayMs5, and its pitch,
  // formant, contour and jitter take the same WIDTH factor. The fields below it shape the person.
  bool voice5On = false;
  float voice5Db = -9.0f;       // -40..0
  float delayMs5 = 65.0f;       // 5..200
  float v5PitchSt = 0.0f;       // -12..12
  float v5FormantSt = 0.0f;     // -6..6
  float v5Contour = 1.0f;       // 0.6..1.4
  float v5TiltDb = 0.0f;        // -6..6, high shelf up and low shelf down by half each at 1 kHz
  float v5Breath = 0.0f;        // 0..0.3, noise under the copy
  float v5Fry = 0.0f;           // 0..1, vocal fry at phrase ends; needs epochAlign
  float v5PeakHz = 3000.0f;     // 2000..4000, the third formant
  float v5PeakDb = 0.0f;        // -6..6
  float v5NotchHz = 1200.0f;    // 800..2000, a nasal anti-resonance
  float v5NotchDb = 0.0f;       // -12..0
  float v5Jitter = 0.0f;        // 0..1 %, local jitter of the cycle length
  float v5Shimmer = 0.0f;       // 0..1 dB, local shimmer of the cycle loudness
};

struct CrowdParams {
  bool on = false;
  float width = 1.0f;   // knob A, 0..1
  float voices = 0.5f;  // knob B, 0..1
  CrowdTuning tuning;

  // A bit per voice (bit 0 is voice 0, bit 4 is voice 5), 0 for no solo. A soloed voice sits fully
  // faded in and the others are silent, whatever the VOICES position. The lead gain follows from the
  // soloed power. Bit 4 counts only while voice5On is set. This is a tuning hook for the emulator. The
  // firmware never sets it.
  uint8_t solo = 0;
};

// Voice 5's neutral shape and its three presets. Each returns a copy of t with the shape fields set and
// leaves voice5On, voice5Db and delayMs5 alone. A preset starts from the neutral shape, so applying
// one after another keeps nothing of the last.
inline CrowdTuning voice5Neutral(CrowdTuning t) {
  const CrowdTuning d;
  t.v5PitchSt = d.v5PitchSt;
  t.v5FormantSt = d.v5FormantSt;
  t.v5Contour = d.v5Contour;
  t.v5TiltDb = d.v5TiltDb;
  t.v5Breath = d.v5Breath;
  t.v5Fry = d.v5Fry;
  t.v5PeakHz = d.v5PeakHz;
  t.v5PeakDb = d.v5PeakDb;
  t.v5NotchHz = d.v5NotchHz;
  t.v5NotchDb = d.v5NotchDb;
  t.v5Jitter = d.v5Jitter;
  t.v5Shimmer = d.v5Shimmer;
  return t;
}

// A woman: a shorter throat with the higher pitch, a brighter tilt, a little breath, a livelier contour.
inline CrowdTuning voice5Woman(CrowdTuning t) {
  t = voice5Neutral(t);
  t.v5PitchSt = 6.0f;
  t.v5FormantSt = 2.75f;
  t.v5TiltDb = 2.0f;
  t.v5Breath = 0.08f;
  t.v5Contour = 1.2f;
  t.v5Jitter = 0.3f;
  t.v5Shimmer = 0.3f;
  return t;
}

// An older man: lower, a longer throat, lax and breathy, with fry at the ends of phrases and a rougher
// cycle.
inline CrowdTuning voice5OlderMan(CrowdTuning t) {
  t = voice5Neutral(t);
  t.v5PitchSt = -2.0f;
  t.v5FormantSt = -1.0f;
  t.v5TiltDb = -3.0f;
  t.v5Fry = 0.5f;
  t.v5Breath = 0.1f;
  t.v5Jitter = 0.6f;
  t.v5Shimmer = 0.5f;
  return t;
}

// A big man: the lowest pitch and the longest throat, with a dark tilt.
inline CrowdTuning voice5BigMan(CrowdTuning t) {
  t = voice5Neutral(t);
  t.v5PitchSt = -4.0f;
  t.v5FormantSt = -2.5f;
  t.v5TiltDb = -4.0f;
  t.v5Jitter = 0.3f;
  return t;
}

namespace crowd_detail {

constexpr size_t kVoices = 4;

// Voice 5 is copy index kVoices. It sits outside the VOICES law, so the law and the tone shelves still
// count kVoices, and everything a copy owns counts kCopies.
constexpr size_t kCopies = kVoices + 1;
constexpr size_t kTrial = kVoices;

constexpr float kSmpPerMs = kSampleRate / 1000.0f;

constexpr float kMinPeriod = 48.0f;
constexpr float kMaxPeriod = 686.0f;
constexpr float kMinRatio = 0.5f;
constexpr float kMaxRatio = 2.0f;
constexpr float kMinRate = 0.5f;
constexpr float kMaxRate = 2.0f;

// A grain never reads closer to the head than this, or further back than the ring holds.
constexpr float kMinReadDelay = 3.0f;
constexpr float kMaxReadDelay = kVoiceRingLen - 4.0f;

// The output lags the input by the read delay. A grain centred on a mark needs one and a half
// periods of lookahead, so the delay never drops below that plus a pad, and never rises past what
// leaves room in the ring for the grain's reach and drift.
constexpr float kFloorPeriods = 1.5f;
constexpr float kFloorPad = 4.0f;
constexpr float kDelayCeil = kVoiceRingLen - 2.0f * kMaxPeriod - 8.0f;

constexpr int kMaxMarkSteps = 128;
constexpr int kQuietBlocks = 2;        // blocks under the gap floor before a copy changes its lag
constexpr float kDriftSlewS = 1.0f;    // time constant of the drift toward its target
constexpr float kMinDriftHoldS = 0.05f;

// The ranges the human-voice controls keep to. The engine clamps to them.
constexpr float kMaxLevelScatterDb = 4.0f;
constexpr float kMinContour = 0.6f;
constexpr float kMaxContour = 1.4f;

// The ranges of voice 5's controls. The engine clamps to them.
constexpr float kMinV5Db = -40.0f;
constexpr float kMinV5DelayMs = 5.0f;
constexpr float kMaxV5DelayMs = 200.0f;
constexpr float kMaxV5PitchSt = 12.0f;
constexpr float kMaxV5FormantSt = 6.0f;
constexpr float kMaxV5TiltDb = 6.0f;
constexpr float kMaxV5Breath = 0.3f;
constexpr float kMinV5PeakHz = 2000.0f;
constexpr float kMaxV5PeakHz = 4000.0f;
constexpr float kMaxV5PeakDb = 6.0f;
constexpr float kMinV5NotchHz = 800.0f;
constexpr float kMaxV5NotchHz = 2000.0f;
constexpr float kMinV5NotchDb = -12.0f;
constexpr float kMaxV5JitterPct = 1.0f;
constexpr float kMaxV5ShimmerDb = 1.0f;
constexpr float kFryLoudnessDb = 3.0f;  // the peak swing of a kept pulse at v5Fry 1

// Samples in a whole number of milliseconds.
constexpr int ms(int v) { return v * kSampleRate / 1000; }

static_assert(kDelayCeil > kFloorPeriods * kMaxPeriod + kFloorPad, "no room for a read delay");
static_assert(ms(200) <= kDelayCeil, "a 200 ms lag and the longest grain do not fit the ring");

// Clamps a lag to what the voice can play at this pitch period.
inline float clampLag(float lag, float period) {
  const float p = clampf(period, kMinPeriod, kMaxPeriod);
  return clampf(lag, kFloorPeriods * p + kFloorPad, kDelayCeil);
}

// The factor WIDTH puts on a copy's pitch and formant offsets. It is linear from (0, 0) to
// (knee, kneeFrac) and on to (1, 1), so it is exactly 0 at WIDTH 0 and exactly 1 at WIDTH 1.
constexpr float kMinWidthKnee = 0.02f;
constexpr float kMaxWidthKnee = 0.9f;

inline float widthFactor(float width, float knee, float kneeFrac) {
  const float k = clampf(knee, kMinWidthKnee, kMaxWidthKnee);
  const float f = clampf(kneeFrac, 0.0f, 1.0f);
  if (width <= k) return f * (width / k);
  const float t = (width - k) / (1.0f - k);
  return f * (1.0f - t) + t;
}

// The multiplier WIDTH puts on every lag target. The longest base lag is delayMs times lagScale; at
// WIDTH 1 it grows by widthLagMs and the rest grow by the same ratio.
constexpr float kMinLagBottom = 0.2f;
constexpr float kMaxLagBottom = 1.0f;
constexpr float kMaxWidthLagMs = 60.0f;

inline float lagMultiplier(const CrowdTuning& t, float width) {
  float longest = 0.0f;
  for (float d : t.delayMs) longest = d * t.lagScale > longest ? d * t.lagScale : longest;
  const float bottom = clampf(t.widthLagBottom, kMinLagBottom, kMaxLagBottom);
  const float top = longest > 0.0f ? 1.0f + clampf(t.widthLagMs, 0.0f, kMaxWidthLagMs) / longest : 1.0f;
  return bottom * (1.0f - width) + top * width;
}

// One PSOLA voice with a grain playback rate. Grains two periods long are cut on a grid one period
// apart and laid down p / ratio apart. A grain plays its input at `rate` samples per output
// sample, so its spectrum moves by `rate` while the pitch follows `ratio`. The read position
// advances by rate per sample and the grain lasts 2p / rate, so it always covers 2p of input.
class FormantPsolaVoice {
 public:
  // About 2 * ratio / rate grains overlap. Eight cover every ratio and rate the tuning and
  // Autotune's pull reach.
  static constexpr size_t kMaxGrains = 8;

  void reset() {
    count_ = 0;
    voiced_ = false;
    fresh_ = true;
    markDelay_ = 0.0f;
    countdown_ = 0.0f;
  }

  // The next prepare() puts the marks at its lag. Grains in flight finish where they are.
  void markReset() { fresh_ = true; }

  // The grain playback rate the last prepare() took, for the tests.
  float rate() const { return rate_; }

  // Per block. period 0 silences the voice. delay is the wanted lag of the output behind the
  // input in samples. The marks sit one period closer than that, because a grain starts one
  // period before its mark.
  void prepare(float period, float ratio, float rate, float delay) {
    voiced_ = period > 0.0f;
    if (!voiced_) {
      count_ = 0;
      return;
    }
    p_ = clampf(period, kMinPeriod, kMaxPeriod);
    rate_ = clampf(rate, kMinRate, kMaxRate);
    step_ = static_cast<int>(rate_ * static_cast<float>(1 << kFracBits) + 0.5f);
    const float r = clampf(ratio, kMinRatio, kMaxRatio);
    plain_ = r == 1.0f && rate_ == 1.0f;
    outPeriod_ = p_ / r;
    dur_ = 2.0f * p_ / rate_;
    amp_ = 0.5f * rate_ / r;
    limit_ = luck_ ? outPeriod_ * luck_->maxScale() : outPeriod_;
    drift_ = (1.0f - rate_) * dur_;
    ref_ = clampLag(delay, p_) - p_;
    if (fresh_) {
      markDelay_ = ref_;
      countdown_ = 0.0f;
      fresh_ = false;
    }
  }

  // The pulses to cut on, or null to cut on the period grid. The tracker must outlive the calls.
  void setEpochs(const EpochTrack* epochs) { epochs_ = epochs; }

  // The random cycle length, grain loudness and dropped pulses of the trial voice, or null for none.
  // Set it before prepare(), which sizes the launch clock to the longest cycle luck can draw.
  void setLuck(GrainLuck* luck) { luck_ = luck; }

  // Adds n samples of the voice into out. first is the ring index of the block's first sample;
  // the voice reads nothing newer than it. clock is the epoch clock of that sample. Launches are
  // placed sample by sample, then each grain plays through the block on its own.
  void render(const VoiceRing& ring, int first, int n, float* out, uint32_t clock = 0) {
    if (!voiced_) return;
    clock_ = clock;
    for (size_t k = 0; k < count_; ++k) grains_[k].start = 0;
    for (int i = 0; i < n; ++i) {
      advanceMarks();
      if (countdown_ > limit_) countdown_ = limit_;
      countdown_ -= 1.0f;
      if (countdown_ > 0.0f) continue;
      launch(wrapOnce<kVoiceRingLen>(first + i), -countdown_, i);
      countdown_ += luck_ ? outPeriod_ * luck_->cycleScale() : outPeriod_;
    }
    size_t kept = 0;
    for (size_t k = 0; k < count_; ++k) {
      if (play(ring.data(), grains_[k], out, n)) grains_[kept++] = grains_[k];
    }
    count_ = kept;
  }

 private:
  // The read position is a fixed-point ring index, so a step of rate samples is one integer add.
  // The ring length shifted up must stay inside an int.
  static constexpr int kFracBits = 16;
  static constexpr int kFracMask = (1 << kFracBits) - 1;
  static constexpr int kRingFix = kVoiceRingLen << kFracBits;
  static constexpr float kFracScale = 1.0f / static_cast<float>(1 << kFracBits);

  static_assert(kVoiceRingLen < (1 << (31 - kFracBits)) - 4, "ring index overflows the fixed-point position");

  struct Grain {
    HannRotor window;
    float age = 0.0f;
    float dur = 0.0f;
    int pos = 0;    // ring index of the next read, in 1 / 2^kFracBits samples
    int step = 0;   // pos advance per output sample
    int start = 0;  // first sample of the block it sounds in
    float gain = 1.0f;  // luck's loudness for this grain, exactly 1 without luck
  };

  // Marks are delays behind the head, kept in [ref_, ref_ + p_).
  void advanceMarks() {
    markDelay_ += 1.0f;
    const float x = markDelay_ - ref_;
    if (x >= p_ || x < 0.0f) realignMarks();
  }

  // Slow path after a wrap or a move of ref_. The loops are bounded, so a wild state cannot hang
  // the audio interrupt.
  void realignMarks() {
    for (int k = 0; k < kMaxMarkSteps && markDelay_ - p_ >= ref_; ++k) markDelay_ -= p_;
    for (int k = 0; k < kMaxMarkSteps && markDelay_ < ref_; ++k) markDelay_ += p_;
  }

  // A free slot, or the oldest grain's when all are in use.
  Grain& nextSlot() {
    if (count_ < kMaxGrains) return grains_[count_++];
    size_t oldest = 0;
    for (size_t k = 1; k < kMaxGrains; ++k) {
      if (grains_[k].age > grains_[oldest].age) oldest = k;
    }
    return grains_[oldest];
  }

  // lead is how far past the ideal launch instant this tick already is. The grain starts one
  // period before its mark. Its delay moves by 1 - rate per sample, so the clamp keeps both ends
  // of that drift inside the ring.
  void launch(int head, float lead, int at) {
    const float gain = luck_ ? luck_->grainGain() : 1.0f;
    if (gain <= 0.0f) return;
    float centre = markDelay_;
    if (centre - ref_ > 0.5f * p_) centre -= p_;
    if (epochs_ && !plain_) centre = epochs_->snap(clock_ + static_cast<uint32_t>(at), centre, p_);
    const float lo = kMinReadDelay + (drift_ < 0.0f ? -drift_ : 0.0f);
    const float hi = kMaxReadDelay - (drift_ > 0.0f ? drift_ : 0.0f);
    const float delay = clampf(centre + p_ - rate_ * lead, lo, hi);
    const float pos = static_cast<float>(head) - delay;
    const float fl = floorf(pos);
    Grain& g = nextSlot();
    g.pos = (wrapOnce<kVoiceRingLen>(static_cast<int>(fl)) << kFracBits) +
            static_cast<int>((pos - fl) * static_cast<float>(1 << kFracBits));
    g.step = step_;
    g.age = lead;
    g.dur = dur_;
    g.start = at;
    g.gain = gain;
    g.window.init(kTwoPi * lead / dur_, kTwoPi / dur_);
  }

  // Plays the grain's share of this block into out and returns true if it lives on into the next.
  // The loop keeps the window rotor and the read position in locals.
  bool play(const float* ring, Grain& g, float* out, int n) const {
    const int room = n - g.start;
    const float left = g.dur - g.age;
    const int remain = left > 0.0f ? static_cast<int>(ceilf(left)) : 0;
    const int m = remain < room ? remain : room;
    HannRotor w = g.window;
    int pos = g.pos;
    const int step = g.step;
    const float amp = amp_ * g.gain;
    float* o = out + g.start;
    for (int j = 0; j < m; ++j) {
      const int i0 = pos >> kFracBits;
      const int i1 = i0 + 1 == kVoiceRingLen ? 0 : i0 + 1;
      const float x0 = ring[i0];
      const float t = static_cast<float>(pos & kFracMask) * kFracScale;
      o[j] += amp * (1.0f - w.c) * (x0 + t * (ring[i1] - x0));
      w.advance();
      pos += step;
      if (pos >= kRingFix) pos -= kRingFix;
    }
    g.window = w;
    g.pos = pos;
    g.age += static_cast<float>(m);
    return remain > room;
  }

  std::array<Grain, kMaxGrains> grains_{};
  size_t count_ = 0;
  int step_ = 1 << kFracBits;
  float p_ = kMinPeriod;
  float rate_ = 1.0f;
  float outPeriod_ = kMinPeriod;
  float limit_ = kMinPeriod;  // the longest the launch clock may run, outPeriod_ unless luck stretches a cycle
  float dur_ = 2.0f * kMinPeriod;
  float amp_ = 0.5f;  // half the grain gain, rate / ratio, so the window needs no 0.5
  float drift_ = 0.0f;
  float ref_ = 0.0f;
  float markDelay_ = 0.0f;
  float countdown_ = 0.0f;
  const EpochTrack* epochs_ = nullptr;
  GrainLuck* luck_ = nullptr;
  uint32_t clock_ = 0;  // epoch clock of the block's first sample
  bool voiced_ = false;
  bool fresh_ = true;
  bool plain_ = false;  // ratio and rate are both exactly 1, so the marks stay on the grid
};

// A copy's slow pitch walk in cents. A new random target in [-peak, +peak] comes every hold, and the
// value slews toward it. A peak of 0 holds the value at exactly 0.
class PitchDrift {
 public:
  void reset() {
    cents_ = 0.0f;
    unit_ = 0.0f;
    left_ = 0.0f;
  }

  // slew is the one-pole coefficient for a block of n samples.
  float step(XorShift& rng, float peak, float holdS, float slew, int n) {
    left_ -= static_cast<float>(n) * (1.0f / kSampleRate);
    if (left_ <= 0.0f) {
      unit_ = rng.next();
      left_ += holdS > kMinDriftHoldS ? holdS : kMinDriftHoldS;
    }
    cents_ = smooth::step(cents_, peak > 0.0f ? unit_ * peak : 0.0f, slew);
    return cents_;
  }

  float cents() const { return cents_; }

 private:
  float cents_ = 0.0f;
  float unit_ = 0.0f;
  float left_ = 0.0f;
};

// Adam's pitch against his own running mean, in octaves. The grains of every copy are laid down at the
// tracker's current period, so a copy's pitch follows the deviation of the moment and not the one its
// lag ago, and nothing is kept but the mean.
class ContourTrack {
 public:
  void reset() {
    mean_ = 0.0f;
    dev_ = 0.0f;
    have_ = false;
  }

  // Per block. period is the tracker's (held through unvoiced frames, 0 before the first voiced
  // one), voiced says the frame is fresh, and slew is the one-pole coefficient of the mean for the
  // block. The mean moves only while the voice is voiced and holds through the rest. The first
  // voiced frame sets it, so the deviation starts at 0.
  void step(float period, bool voiced, float slew) {
    if (!(period > 0.0f)) return;
    const float l = log2Fast(period);
    if (voiced) {
      mean_ = have_ ? mean_ + slew * (l - mean_) : l;
      have_ = true;
    }
    dev_ = have_ ? mean_ - l : 0.0f;
  }

  // Octaves above the mean: positive when the voice is sharper than its average.
  float deviation() const { return dev_; }

 private:
  float mean_ = 0.0f;  // log2 of the period, averaged
  float dev_ = 0.0f;
  bool have_ = false;
};

// The block being prepared, as the gap rule sees it.
struct BlockView {
  const VoiceRing& ring;
  int first;          // ring index of the block's first sample
  int n;
  float quietSq;      // a mean square under this counts as quiet

  // True when the n samples that the ring holds `lag` behind the block are quiet.
  bool quietAt(float lag) const {
    int at = (first - static_cast<int>(lag + 0.5f)) % kVoiceRingLen;
    if (at < 0) at += kVoiceRingLen;
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) {
      const float x = ring[static_cast<size_t>(at)];
      sum += x * x;
      at = at + 1 == kVoiceRingLen ? 0 : at + 1;
    }
    return sum < quietSq * static_cast<float>(n);
  }
};

// One copy's lag. A new target (a tuning move, or the scatter draw of a fresh gap)
// lands once the ring is quiet at both the lag the copy plays and the lag it moves to. The jump
// then replays or skips only silence. While material sounds the copy keeps the lag it has.
class CopyLag {
 public:
  void seed(uint32_t s) {
    rng_.seed(s);
    offset_ = rng_.next();
    level_ = 0.0f;
  }

  // Unsets the lag and keeps the random stream.
  void restart() {
    set_ = false;
    landed_ = false;
    quiet_ = 0;
  }

  float samples() const { return lag_; }

  // The loudness draw of the current gap, -1..1. It is 0 until a gap has drawn one, and it only
  // draws while level scatter is on.
  float levelUnit() const { return level_; }

  // Per block. base is the target in samples before scatter. rollLevel says level scatter is on,
  // so each gap also draws a loudness, which keeps the gap rule running when the timing scatter is
  // 0. True when the lag landed on a new value, so the voice restarts its marks there.
  bool settle(const BlockView& b, float base, float scatterSmp, float period, bool rollLevel) {
    const float want = clampLag(base + offset_ * scatterSmp, period);
    if (!set_) return land(base, want, rollLevel);
    const bool moved = base != base_;
    if (!moved && scatterSmp <= 0.0f && !rollLevel) {
      quiet_ = 0;
      landed_ = false;
      return false;
    }
    const bool here = b.quietAt(clampLag(lag_, period));
    if (!here) landed_ = false;
    const bool pending = moved || !landed_;
    const bool calm = here && (!pending || b.quietAt(want));
    quiet_ = calm ? quiet_ + 1 : 0;
    return quiet_ >= kQuietBlocks && pending && land(base, want, rollLevel);
  }

 private:
  bool land(float base, float want, bool rollLevel) {
    lag_ = want;
    base_ = base;
    set_ = true;
    landed_ = true;
    quiet_ = 0;
    offset_ = rng_.next();
    if (rollLevel) level_ = rng_.next();
    return true;
  }

  XorShift rng_;
  float offset_ = 0.0f;  // the next scatter draw, -1..1
  float level_ = 0.0f;   // this gap's loudness draw, -1..1
  float lag_ = 0.0f;
  float base_ = 0.0f;
  int quiet_ = 0;
  bool set_ = false;
  bool landed_ = false;  // this gap has taken its landing
};

// The VOICES law at knob position k. weight[v] is the voice's fixed gain times its fade, weight5 the
// trial voice's fixed gain, lead the lead's gain, mul the crowd multiplier that carries the top of the
// knob.
struct VoicesLaw {
  std::array<float, kVoices> weight{};
  float weight5 = 0.0f;
  float lead = 1.0f;
  float mul = 1.0f;
};

// Voice 5 sounds at its own level whatever k is, unless another voice is soloed alone.
inline float trialWeight(const CrowdTuning& t, bool soloing, bool solo5) {
  if (!t.voice5On || (soloing && !solo5)) return 0.0f;
  return dbToGainFast(clampf(t.voice5Db, kMinV5Db, 0.0f));
}

// solo is a bit per voice. When a bit in 0..3 is set, each soloed voice takes fade 1 and the others
// 0 at every k, and the rest of the law runs on that crowd. The lead and the crowd multiplier then
// follow from the soloed power, so a single copy at the top of the knob stands alone at unity. Bit 4
// solos voice 5 while it is on. Voice 5 adds its power to the crowd's and holds its gain at every k.
inline VoicesLaw voicesLaw(float k, const CrowdTuning& t, uint8_t solo = 0) {
  VoicesLaw law;
  const float g = dbToGainFast(t.voiceDb);
  const float from = clampf(t.leadFadeFrom, 0.05f, 0.99f);
  const float hold = clampf(t.holdFrom, 0.05f, 1.0f);
  const float u = k / (from * 0.25f);
  const unsigned mask = solo & 0x0Fu;
  const bool solo5 = t.voice5On && (solo & 0x10u) != 0;
  const bool soloing = mask != 0 || solo5;
  law.weight5 = trialWeight(t, soloing, solo5);
  unsigned count = 0;
  float sumSq = 0.0f;
  for (size_t v = 0; v < kVoices; ++v) {
    const unsigned bit = (mask >> v) & 1u;
    count += bit;
    const float x = smooth::clamp01((u - static_cast<float>(v)) / hold);
    const float fade = soloing ? static_cast<float>(bit) : sinf(kHalfPi * x);
    law.weight[v] = g * fade;
    sumSq += law.weight[v] * law.weight[v];
  }
  sumSq += law.weight5 * law.weight5;
  if (k <= from) {
    law.lead = sqrtf(sumSq < 1.0f ? 1.0f - sumSq : 0.0f);
    return law;
  }

  // Past the fade point every sounding voice holds at g. Crowd power runs from the sounding voices'
  // power (four voices without a solo) up to unity while the lead falls to nothing, and the crowd
  // multiplier makes up the difference.
  const float s = smooth::clamp01((k - from) / (1.0f - from));
  const float p0 = static_cast<float>(soloing ? count : kVoices) * g * g + law.weight5 * law.weight5;
  const float sn = sinf(kHalfPi * s);
  const float power = s >= 1.0f ? 1.0f : p0 + (1.0f - p0) * sn * sn;
  for (size_t v = 0; v < kVoices; ++v) law.weight[v] = !soloing || ((mask >> v) & 1u) ? g : 0.0f;
  law.lead = sqrtf(power < 1.0f ? 1.0f - power : 0.0f);
  law.mul = p0 > 1e-6f ? sqrtf(power / p0) : 1.0f;
  return law;
}

}  // namespace crowd_detail

class Crowd {
 public:
  // The constructor runs reset(), so the non-zero defaults below land in .bss with the rest.
  Crowd() { reset(); }

  void reset() {
    clearDsp();
    gain_.fill(0.0f);
    lead_ = 1.0f;
    wet_ = 0.0f;
    for (size_t v = 0; v < kCopies; ++v) {
      lag_[v].seed(0x2545F491u * static_cast<uint32_t>(v + 1));
      driftRng_[v].seed(0x9E3779B9u * static_cast<uint32_t>(v + 1) ^ 0x85EBCA6Bu);
    }
    idle_ = true;
  }

  // in is this slot's input, the lead. pitch has already run this block, so the block's samples
  // sit at ring index head() - n + i. Off and settled, out equals in bit for bit.
  void process(const float* in, float* out, int n, const CrowdParams& p, const PitchFx& pitch) {
    if (settledOff(p)) {
      passThrough(in, out, n);
      return;
    }
    idle_ = false;
    const Plan plan = makePlan(p);
    const int first = pitch.head() - n;
    prepareBlock(p, pitch, plan, in, first, n);
    const uint32_t clock = epochs_.clock() - static_cast<uint32_t>(n);
    for (int c = 0; c < n; c += kBlock) {
      const int m = n - c < kBlock ? n - c : kBlock;
      renderChunk(in + c, out + c, plan, Chunk{pitch.ring(), first + c, m, clock + static_cast<uint32_t>(c)});
    }
  }

  // Test hooks. leadGain is the lead's gain at the end of the last block. activeVoices counts the
  // voices, voice 5 among them, that ticked in it. lagSamples, driftCents and grainRate read a copy's
  // current lag, pitch drift and grain playback rate; copy 4 is voice 5.
  float leadGain() const { return lead_; }
  uint8_t activeVoices() const { return active_; }
  float lagSamples(size_t v) const { return lag_[v].samples(); }
  float driftCents(size_t v) const { return drift_[v].cents(); }
  float grainRate(size_t v) const { return voice_[v].rate(); }

  // Whether voice 5's fry window was open in the last block.
  bool fryActive() const { return luck_.frying; }

 private:
  using FormantPsolaVoice = crowd_detail::FormantPsolaVoice;
  static constexpr size_t kVoices = crowd_detail::kVoices;
  static constexpr size_t kCopies = crowd_detail::kCopies;
  static constexpr size_t kTrial = crowd_detail::kTrial;

  // Block-end targets for everything the knobs and the tuning set.
  struct Plan {
    std::array<float, kCopies> voice{};  // fixed gain times fade, per copy; copy 4 is voice 5
    float lead = 1.0f;
    float wet = 0.0f;  // crowd multiplier times the trim
  };

  static Plan makePlan(const CrowdParams& p) {
    Plan plan;
    if (!p.on) return plan;
    const CrowdTuning& t = p.tuning;
    const crowd_detail::VoicesLaw law = crowd_detail::voicesLaw(smooth::clamp01(p.voices), t, p.solo);
    for (size_t v = 0; v < kVoices; ++v) plan.voice[v] = law.weight[v];
    plan.voice[kTrial] = law.weight5;
    plan.lead = law.lead;
    plan.wet = law.mul * dbToGainFast(t.trimDb);
    return plan;
  }

  bool settledOff(const CrowdParams& p) const {
    if (p.on || lead_ != 1.0f || wet_ != 0.0f) return false;
    for (float g : gain_) {
      if (g != 0.0f) return false;
    }
    return true;
  }

  void passThrough(const float* in, float* out, int n) {
    if (!idle_) {
      clearDsp();
      idle_ = true;
    }
    for (int i = 0; i < n; ++i) out[i] = in[i];
  }

  void clearDsp() {
    for (size_t v = 0; v < kCopies; ++v) retire(v);
    hp_.clear();
    dc_.clear();
    contour_.reset();
    epochs_.reset();
    active_ = 0;
    tiltSet_ = false;
    widthSet_ = false;
  }

  void retire(size_t v) {
    voice_[v].reset();
    lag_[v].restart();
    drift_[v].reset();
    levelMul_[v] = 1.0f;
    live_[v] = false;
    if (v == kTrial) {
      trial_.clear();
      fry_.reset();
      luck_.seed(0xA511B2D7u);
      luck_.frying = false;
      return;
    }
    tilt_[v].clear();
  }

  // The low cut on the crowd sum. The setter runs on the first block after a reset and when the
  // tuning moved.
  void setLowCut(const CrowdTuning& t) {
    if (hpSet_ && t.hpHz == hpHz_) return;
    hp_.setHighPass(t.hpHz, kButterworthQ);
    hpHz_ = t.hpHz;
    hpSet_ = true;
  }

  // One high shelf per voice, set on the first block after a reset and when the tuning moved.
  void setTilts(const CrowdTuning& t) {
    for (size_t v = 0; v < kVoices; ++v) {
      if (tiltSet_ && t.tiltDb[v] == tiltDb_[v] && t.tiltHz == tiltHz_) continue;
      tilt_[v].setHighShelf(t.tiltHz, t.tiltDb[v]);
      tiltDb_[v] = t.tiltDb[v];
    }
    tiltHz_ = t.tiltHz;
    tiltSet_ = true;
  }

  // What copy v takes from the tuning. Voice 5 reads its own fields, clamped to their ranges.
  static float delayOf(const CrowdTuning& t, size_t v) {
    return v < kVoices ? t.delayMs[v] : clampf(t.delayMs5, crowd_detail::kMinV5DelayMs, crowd_detail::kMaxV5DelayMs);
  }

  static float pitchOf(const CrowdTuning& t, size_t v) {
    return v < kVoices ? t.pitchSt[v] : clampf(t.v5PitchSt, -crowd_detail::kMaxV5PitchSt, crowd_detail::kMaxV5PitchSt);
  }

  static float formantOf(const CrowdTuning& t, size_t v) {
    return v < kVoices ? t.formantSt[v]
                       : clampf(t.v5FormantSt, -crowd_detail::kMaxV5FormantSt, crowd_detail::kMaxV5FormantSt);
  }

  static float contourOf(const CrowdTuning& t, size_t v) { return v < kVoices ? t.contour[v] : t.v5Contour; }

  // Copy v's lag target in samples. The scale and the WIDTH multiplier stretch the delay and the
  // ceiling caps it.
  static float lagTarget(const CrowdTuning& t, size_t v, float widthMul) {
    const float lagMs = delayOf(t, v) * t.lagScale * widthMul;
    return (lagMs < t.maxLagMs ? lagMs : t.maxLagMs) * crowd_detail::kSmpPerMs;
  }

  // What every voice of one block reads.
  struct Block {
    const CrowdTuning& t;
    crowd_detail::BlockView view;
    float period;
    float corr;        // Autotune's correction in octaves
    float width;       // factor on the pitch and formant offsets, 0 at WIDTH 0
    float lagMul;      // WIDTH multiplier on every lag target
    float scatterSmp;  // timing scatter in samples
    float levelDb;     // level scatter, clamped to its range
    float slew;        // one-pole coefficient of the slow walks for this block
    int n;
    const crowd_detail::EpochTrack* epochs;  // null cuts on the period grid
    const float* in;                         // the lead, for voice 5's phrase-end watch
  };

  // The shared pulse marks of this block, or null when the grid cuts. Off drops the marks, so a
  // later on starts a fresh search.
  const crowd_detail::EpochTrack* prepareEpochs(const CrowdTuning& t, const PitchFx& pitch, float period, int n) {
    if (!t.epochAlign) {
      epochs_.reset();
      return nullptr;
    }
    const float p = period > 0.0f ? clampf(period, crowd_detail::kMinPeriod, crowd_detail::kMaxPeriod) : 0.0f;
    epochs_.advance(pitch.shadow(), pitch.head(), n, p, t.epochSearch);
    return &epochs_;
  }

  // The WIDTH the factors read. It follows the knob through the 20 ms one-pole, so a knob step moves
  // the copies' pitch over a few blocks and not in one jump, and it takes the knob value outright on
  // the first block after a reset. A settled value equals the knob bit for bit.
  float smoothedWidth(float width, int n) {
    const float target = smooth::clamp01(width);
    const float a = 1.0f - expf(-static_cast<float>(n) / (kSampleRate * smooth::kSmoothSec));
    widthSm_ = widthSet_ ? smooth::step(widthSm_, target, a) : target;
    widthSet_ = true;
    return widthSm_;
  }

  // Per block: filters, the contour track and the pulse marks, then each live voice. in is the lead
  // and first the ring index of the block's first sample.
  void prepareBlock(const CrowdParams& p, const PitchFx& pitch, const Plan& plan, const float* in, int first,
                    int n) {
    const CrowdTuning& t = p.tuning;
    setLowCut(t);
    setTilts(t);
    const PitchResult& pr = pitch.pitch();
    const float slew = 1.0f - expf(-static_cast<float>(n) / (kSampleRate * crowd_detail::kDriftSlewS));
    contour_.step(pr.period, pr.voiced, slew);
    const crowd_detail::EpochTrack* epochs = prepareEpochs(t, pitch, pr.period, n);
    const Block b{t,
                  {pitch.ring(), first, n, dbToGainFast(2.0f * t.gapFloorDb)},
                  pr.period,
                  pitch.correctionSemis() * (1.0f / 12.0f),
                  crowd_detail::widthFactor(smoothedWidth(p.width, n), t.widthKnee, t.widthKneeFrac),
                  crowd_detail::lagMultiplier(t, smooth::clamp01(p.width)),
                  (t.scatterMs > 0.0f ? t.scatterMs : 0.0f) * crowd_detail::kSmpPerMs,
                  clampf(t.levelScatterDb, 0.0f, crowd_detail::kMaxLevelScatterDb),
                  slew,
                  n,
                  epochs,
                  in};
    active_ = 0;
    for (size_t v = 0; v < kCopies; ++v) prepareVoice(v, plan, b);
  }

  // Copy v's lag, loudness, pitch ratio and playback rate. A voice whose gain sits at 0
  // across the block is cleared and does not tick.
  void prepareVoice(size_t v, const Plan& plan, const Block& b) {
    if (gain_[v] <= 0.0f && plan.voice[v] <= 0.0f) {
      if (live_[v]) retire(v);
      return;
    }
    const CrowdTuning& t = b.t;
    live_[v] = true;
    ++active_;
    if (v == kTrial) prepareTrial(t, b);
    const bool roll = b.levelDb > 0.0f;
    if (lag_[v].settle(b.view, lagTarget(t, v, b.lagMul), b.scatterSmp, b.period, roll)) voice_[v].markReset();
    levelMul_[v] = roll ? dbToGainFast(lag_[v].levelUnit() * b.levelDb) : 1.0f;
    const float drift = drift_[v].step(driftRng_[v], t.driftCents * b.width, t.driftHoldS, b.slew, b.n);
    const float st = pitchOf(t, v) * b.width;
    const float oct = st * (1.0f / 12.0f) + drift * (1.0f / 1200.0f) + b.corr;
    voice_[v].prepare(b.period, exp2Fast(oct + contourOctaves(v, t, b.width)),
                      exp2Fast(formantOf(t, v) * b.width * (1.0f / 12.0f)), lag_[v].samples());
    voice_[v].setEpochs(b.epochs);
  }

  // Voice 5 only. Hands the cutting its jitter, shimmer and fry, and the chain its colour. Fry needs
  // the pulse cut, since it drops pulses, and watches the lead only while it is on.
  void prepareTrial(const CrowdTuning& t, const Block& b) {
    namespace cd = crowd_detail;
    const float fry = clampf(t.v5Fry, 0.0f, 1.0f);
    luck_.jitter = clampf(t.v5Jitter, 0.0f, cd::kMaxV5JitterPct) * 0.01f * b.width;
    luck_.shimmerDb = clampf(t.v5Shimmer, 0.0f, cd::kMaxV5ShimmerDb);
    luck_.dropChance = fry;
    luck_.fryDb = cd::kFryLoudnessDb * fry;
    if (fry > 0.0f && t.epochAlign) {
      luck_.frying = fry_.step(b.in, b.n, contour_.deviation());
    } else {
      fry_.reset();
      luck_.frying = false;
    }
    voice_[kTrial].setLuck(&luck_);
    breath_ = clampf(t.v5Breath, 0.0f, cd::kMaxV5Breath);
    trial_.set(clampf(t.v5TiltDb, -cd::kMaxV5TiltDb, cd::kMaxV5TiltDb),
               clampf(t.v5PeakHz, cd::kMinV5PeakHz, cd::kMaxV5PeakHz),
               clampf(t.v5PeakDb, -cd::kMaxV5PeakDb, cd::kMaxV5PeakDb),
               clampf(t.v5NotchHz, cd::kMinV5NotchHz, cd::kMaxV5NotchHz),
               clampf(t.v5NotchDb, cd::kMinV5NotchDb, 0.0f));
  }

  // How far copy v's pitch moves from the lead's, in octaves: (contour - 1) of Adam's deviation.
  // The copy then sits contour times as far from his mean as he does. Contour 1 adds exactly 0. The
  // WIDTH factor pulls the contour toward 1, so it is exactly 1 at the bottom of WIDTH and the
  // slider value at WIDTH 1.
  float contourOctaves(size_t v, const CrowdTuning& t, float factor) const {
    const float c = clampf(contourOf(t, v), crowd_detail::kMinContour, crowd_detail::kMaxContour);
    return c == 1.0f ? 0.0f : (c - 1.0f) * factor * contour_.deviation();
  }

  // One chunk of the call: kBlock samples at most.
  struct Chunk {
    const VoiceRing& ring;
    int first;       // ring index of the chunk's first sample
    int n;
    uint32_t clock;  // epoch clock of that sample
  };

  // Adds voice v into crowd. The voice's tone shelf runs first, then its gain ramps to the
  // block-end target. The shelf runs on a local copy so its state stays in registers.
  void mixVoice(size_t v, const Chunk& k, float target, float* crowd) {
    if (v == kTrial) {
      mixTrial(k, target, crowd);
      return;
    }
    std::array<float, kBlock> raw{};
    voice_[v].render(k.ring, k.first, k.n, raw.data(), k.clock);
    Biquad tilt = tilt_[v];
    const float step = (target - gain_[v]) / static_cast<float>(k.n);
    float g = gain_[v];
    for (int i = 0; i < k.n; ++i) {
      g += step;
      crowd[i] += g * tilt.run(raw[static_cast<size_t>(i)]);
    }
    tilt_[v] = tilt;
  }

  // Adds voice 5 into crowd. Its chain (tilt, peak, notch, breath) replaces the tone shelf the other
  // four share, then its gain ramps to the block-end target.
  void mixTrial(const Chunk& k, float target, float* crowd) {
    std::array<float, kBlock> raw{};
    voice_[kTrial].render(k.ring, k.first, k.n, raw.data(), k.clock);
    trial_.shape(raw.data(), k.n, breath_);
    const float step = (target - gain_[kTrial]) / static_cast<float>(k.n);
    float g = gain_[kTrial];
    for (int i = 0; i < k.n; ++i) {
      g += step;
      crowd[i] += g * raw[static_cast<size_t>(i)];
    }
  }

  // The low cut on the crowd sum, in place. The filter runs on a local copy so its state stays in
  // registers.
  void filterCrowd(float* crowd, int n) {
    Biquad hp = hp_;
    for (int i = 0; i < n; ++i) crowd[i] = hp.run(crowd[i]);
    hp_ = hp;
  }

  // Lead and crowd ramp linearly from the last chunk's values to the plan. A voice's gain target is
  // its plan gain times the loudness its last gap drew, which is exactly 1 with level scatter off.
  void renderChunk(const float* in, float* out, const Plan& plan, const Chunk& k) {
    std::array<float, kBlock> crowd{};
    for (size_t v = 0; v < kCopies; ++v) {
      if (live_[v]) mixVoice(v, k, plan.voice[v] * levelMul_[v], crowd.data());
    }
    filterCrowd(crowd.data(), k.n);
    const float inv = 1.0f / static_cast<float>(k.n);
    for (int i = 0; i < k.n; ++i) {
      const float f = static_cast<float>(i + 1) * inv;
      const float wet = dc_.run(crowd[static_cast<size_t>(i)]);
      out[i] = (lead_ + (plan.lead - lead_) * f) * in[i] + (wet_ + (plan.wet - wet_) * f) * wet;
    }
    for (size_t v = 0; v < kCopies; ++v) gain_[v] = plan.voice[v] * levelMul_[v];
    lead_ = plan.lead;
    wet_ = plan.wet;
  }

  // Copies 0..3 are the VOICES law's; copy 4 is voice 5.
  std::array<FormantPsolaVoice, kCopies> voice_{};
  Biquad hp_;
  std::array<Biquad, kVoices> tilt_{};
  std::array<float, kVoices> tiltDb_{};
  std::array<float, kCopies> gain_{};
  std::array<bool, kCopies> live_{};
  std::array<crowd_detail::CopyLag, kCopies> lag_{};
  std::array<crowd_detail::PitchDrift, kCopies> drift_{};
  std::array<crowd_detail::XorShift, kCopies> driftRng_{};
  std::array<float, kCopies> levelMul_ = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  crowd_detail::ContourTrack contour_;
  crowd_detail::EpochTrack epochs_;
  crowd_detail::TrialChain trial_;
  crowd_detail::GrainLuck luck_;
  crowd_detail::FryGate fry_;
  DcBlocker dc_;
  float lead_ = 1.0f;
  float wet_ = 0.0f;
  float hpHz_ = 0.0f;
  float tiltHz_ = 0.0f;
  float breath_ = 0.0f;
  float widthSm_ = 1.0f;
  uint8_t active_ = 0;
  bool widthSet_ = false;
  bool hpSet_ = false;
  bool tiltSet_ = false;
  bool idle_ = true;
};

}  // namespace cv
