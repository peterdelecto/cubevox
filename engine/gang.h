#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/gang_epoch.h"
#include "engine/pitch_fx.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// The Gang card adds up to four copies of the voice, each a different person saying the same line
// a little after Adam. Each copy sits a few semitones off the lead, has its own throat (a grain
// playback rate) and its own tone shelf, and lags the lead by its own delay. The gang as a whole
// has a fixed low cut. The copies never read a key. They read the pitch period and Autotune's
// correction from PitchFx and cut their grains from its input ring.
//
// VOICES fades the copies in one at a time (low, high, low, high), then fades the lead out, so the
// top of the knob is the gang alone. Total power is unity at every position. WIDTH spreads the
// copies' pitch offsets from a hair off the lead to their full values and moves nothing else.
// A copy changes its lag only in a quiet gap, so it never bends in pitch mid-word. Scatter gives
// each copy a fresh timing offset at every gap, and drift walks each copy's pitch slowly around
// its offset.
//
// Four more controls make a copy read as a person and not as Adam shifted. Each is neutral at its
// default, and the output is then bit for bit the output without it. Level scatter gives a copy a
// new loudness at every gap. Contour sets how much of Adam's rises and falls a copy follows. Breath
// adds air under a copy. Epoch align cuts every grain on a glottal pulse instead of on the period
// grid, which removes the buzz and the doubled pulses a grid cut leaves when a copy's throat moves.
// All four stay inside what a human voice does, and the engine clamps each to that range.

namespace cv {

struct GangTuning {
  std::array<float, 4> pitchSt = {-1.75f, 2.75f, -3.0f, 1.0f};     // full pitch offset 2^(st / 12) at WIDTH 1
  std::array<float, 4> formantSt = {-1.75f, -1.0f, -1.0f, -1.5f};  // grain playback rate 2^(st / 12)
  std::array<float, 4> tiltDb = {-3.0f, -4.0f, 3.0f, 5.0f};        // high shelf gain above tiltHz
  std::array<float, 4> delayMs = {40.0f, 100.0f, 70.0f, 75.0f};    // lag behind the lead before lagScale
  float lagScale = 1.2f;       // a copy's lag is delayMs times this
  float tiltHz = 2500.0f;
  float voiceDb = -9.0f;       // one voice against the lead at unity
  float trimDb = 2.1f;         // level rule residue
  float leadFadeFrom = 0.8f;   // VOICES position where the lead starts to leave
  float holdFrom = 0.75f;      // part of each voice's fifth spent fading; the rest holds
  float hpHz = 120.0f;         // low cut on the gang sum
  float maxLagMs = 200.0f;     // ceiling on a copy's lag
  float gapFloorDb = -50.0f;   // a copy's read point under this level counts as a gap
  float scatterMs = 0.0f;      // peak random lag offset a copy takes at each gap
  float driftCents = 0.0f;     // peak of each copy's slow random pitch walk
  float driftHoldS = 2.0f;     // time between new drift targets

  // Each copy takes a new loudness at every gap, uniform in +- this many dB (0..4).
  float levelScatterDb = 0.0f;

  // The share of Adam's pitch deviation a copy follows (0.6..1.4). 1 follows him exactly, 0.7 flattens
  // the copy's rises and falls to 70 % of his, 1.3 exaggerates them.
  std::array<float, 4> contour = {1.0f, 1.0f, 1.0f, 1.0f};

  // Breath noise under each copy (0..0.15), high-passed at breathHz and shaped by the copy's own
  // envelope. 0 leaves the copy as it is.
  std::array<float, 4> breath = {0.0f, 0.0f, 0.0f, 0.0f};
  float breathHz = 2000.0f;

  // True cuts each grain on the glottal pulse nearest its grid mark. epochSearch is the half-width
  // in periods of the peak search around each expected pulse (0..0.3).
  bool epochAlign = false;
  float epochSearch = 0.25f;
};

struct GangParams {
  bool on = false;
  float width = 1.0f;   // knob A, 0..1
  float voices = 0.5f;  // knob B, 0..1
  GangTuning tuning;

  // A bit per voice (bit 0 is voice 0), 0 for no solo. A soloed voice sits fully faded in and the
  // others are silent, whatever the VOICES position. The lead gain follows from the soloed power.
  // This is a tuning hook for the emulator. The firmware never sets it.
  uint8_t solo = 0;
};

namespace gang_detail {

constexpr size_t kVoices = 4;
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

// The ranges the four human-voice controls keep to. The engine clamps to them.
constexpr float kMaxLevelScatterDb = 4.0f;
constexpr float kMinContour = 0.6f;
constexpr float kMaxContour = 1.4f;
constexpr float kMaxBreath = 0.15f;

// Breath follows the copy's level with these one-pole coefficients, computed once at start.
inline const float kBreathAttack = smooth::coef(0.010f);
inline const float kBreathRelease = smooth::coef(0.060f);

// Samples in a whole number of milliseconds.
constexpr int ms(int v) { return v * kSampleRate / 1000; }

static_assert(kDelayCeil > kFloorPeriods * kMaxPeriod + kFloorPad, "no room for a read delay");
static_assert(ms(200) <= kDelayCeil, "a 200 ms lag and the longest grain do not fit the ring");

// Clamps a lag to what the voice can play at this pitch period.
inline float clampLag(float lag, float period) {
  const float p = clampf(period, kMinPeriod, kMaxPeriod);
  return clampf(lag, kFloorPeriods * p + kFloorPad, kDelayCeil);
}

// A copy's pitch offset in semitones at WIDTH `width`. At 0 it sits kWidthFloorSt off the lead on its
// own side, at 1 it sits at its full offset, and it moves linearly between. An offset already
// inside the floor stays as it is, so an offset of 0 stays on the lead.
constexpr float kWidthFloorSt = 0.01f;

inline float widthSt(float full, float width) {
  const float a = full < 0.0f ? -full : full;
  if (a <= kWidthFloorSt) return full;
  const float st = kWidthFloorSt + (a - kWidthFloorSt) * width;
  return full < 0.0f ? -st : st;
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
    outPeriod_ = p_ / r;
    dur_ = 2.0f * p_ / rate_;
    amp_ = 0.5f * rate_ / r;
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

  // Adds n samples of the voice into out. first is the ring index of the block's first sample;
  // the voice reads nothing newer than it. clock is the epoch clock of that sample. Launches are
  // placed sample by sample, then each grain plays through the block on its own.
  void render(const VoiceRing& ring, int first, int n, float* out, uint32_t clock = 0) {
    if (!voiced_) return;
    clock_ = clock;
    for (size_t k = 0; k < count_; ++k) grains_[k].start = 0;
    for (int i = 0; i < n; ++i) {
      advanceMarks();
      if (countdown_ > outPeriod_) countdown_ = outPeriod_;
      countdown_ -= 1.0f;
      if (countdown_ > 0.0f) continue;
      launch(wrapOnce<kVoiceRingLen>(first + i), -countdown_, i);
      countdown_ += outPeriod_;
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
    float centre = markDelay_;
    if (centre - ref_ > 0.5f * p_) centre -= p_;
    if (epochs_) centre = epochs_->snap(clock_ + static_cast<uint32_t>(at), centre, p_);
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
    float* o = out + g.start;
    for (int j = 0; j < m; ++j) {
      const int i0 = pos >> kFracBits;
      const int i1 = i0 + 1 == kVoiceRingLen ? 0 : i0 + 1;
      const float x0 = ring[i0];
      const float t = static_cast<float>(pos & kFracMask) * kFracScale;
      o[j] += amp_ * (1.0f - w.c) * (x0 + t * (ring[i1] - x0));
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
  float dur_ = 2.0f * kMinPeriod;
  float amp_ = 0.5f;  // half the grain gain, rate / ratio, so the window needs no 0.5
  float drift_ = 0.0f;
  float ref_ = 0.0f;
  float markDelay_ = 0.0f;
  float countdown_ = 0.0f;
  const EpochTrack* epochs_ = nullptr;
  uint32_t clock_ = 0;  // epoch clock of the block's first sample
  bool voiced_ = false;
  bool fresh_ = true;
};

// A small random source that repeats from its seed. Each voice owns one per use.
class XorShift {
 public:
  void seed(uint32_t s) { s_ = s != 0u ? s : 1u; }

  // Returns a uniform value in [-1, 1].
  float next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return static_cast<float>(static_cast<int32_t>(s_)) * (1.0f / 2147483648.0f);
  }

 private:
  uint32_t s_ = 1u;
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

// The VOICES law at knob position k. weight[v] is the voice's fixed gain times its fade, lead the
// lead's gain, mul the gang multiplier that carries the top of the knob.
struct VoicesLaw {
  std::array<float, kVoices> weight{};
  float lead = 1.0f;
  float mul = 1.0f;
};

// solo is a bit per voice. When a bit in 0..3 is set, each soloed voice takes fade 1 and the others
// 0 at every k, and the rest of the law runs on that gang. The lead and the gang multiplier then
// follow from the soloed power, so a single copy at the top of the knob stands alone at unity.
inline VoicesLaw voicesLaw(float k, const GangTuning& t, uint8_t solo = 0) {
  VoicesLaw law;
  const float g = dbToGainFast(t.voiceDb);
  const float from = clampf(t.leadFadeFrom, 0.05f, 0.99f);
  const float hold = clampf(t.holdFrom, 0.05f, 1.0f);
  const float u = k / (from * 0.25f);
  const unsigned mask = solo & 0x0Fu;
  unsigned count = 0;
  float sumSq = 0.0f;
  for (size_t v = 0; v < kVoices; ++v) {
    const unsigned bit = (mask >> v) & 1u;
    count += bit;
    const float x = smooth::clamp01((u - static_cast<float>(v)) / hold);
    const float fade = mask == 0 ? sinf(kHalfPi * x) : static_cast<float>(bit);
    law.weight[v] = g * fade;
    sumSq += law.weight[v] * law.weight[v];
  }
  if (k <= from) {
    law.lead = sqrtf(sumSq < 1.0f ? 1.0f - sumSq : 0.0f);
    return law;
  }

  // Past the fade point every sounding voice holds at g. Gang power runs from the sounding voices'
  // power (four voices without a solo) up to unity while the lead falls to nothing, and the gang
  // multiplier makes up the difference.
  const float s = smooth::clamp01((k - from) / (1.0f - from));
  const float p0 = static_cast<float>(mask == 0 ? kVoices : count) * g * g;
  const float sn = sinf(kHalfPi * s);
  const float power = s >= 1.0f ? 1.0f : p0 + (1.0f - p0) * sn * sn;
  for (size_t v = 0; v < kVoices; ++v) law.weight[v] = mask == 0 || ((mask >> v) & 1u) ? g : 0.0f;
  law.lead = sqrtf(power < 1.0f ? 1.0f - power : 0.0f);
  law.mul = p0 > 1e-6f ? sqrtf(power / p0) : 1.0f;
  return law;
}

}  // namespace gang_detail

class Gang {
 public:
  // The constructor runs reset(), so the non-zero defaults below land in .bss with the rest.
  Gang() { reset(); }

  void reset() {
    clearDsp();
    gain_.fill(0.0f);
    lead_ = 1.0f;
    wet_ = 0.0f;
    for (size_t v = 0; v < kVoices; ++v) {
      lag_[v].seed(0x2545F491u * static_cast<uint32_t>(v + 1));
      driftRng_[v].seed(0x9E3779B9u * static_cast<uint32_t>(v + 1) ^ 0x85EBCA6Bu);
      breath_[v].rng.seed(0xC2B2AE35u * static_cast<uint32_t>(v + 1) ^ 0x27D4EB2Fu);
    }
    idle_ = true;
  }

  // in is this slot's input, the lead. pitch has already run this block, so the block's samples
  // sit at ring index head() - n + i. Off and settled, out equals in bit for bit.
  void process(const float* in, float* out, int n, const GangParams& p, const PitchFx& pitch) {
    if (settledOff(p)) {
      passThrough(in, out, n);
      return;
    }
    idle_ = false;
    const Plan plan = makePlan(p);
    const int first = pitch.head() - n;
    prepareBlock(p, pitch, plan, first, n);
    const uint32_t clock = epochs_.clock() - static_cast<uint32_t>(n);
    for (int c = 0; c < n; c += kBlock) {
      const int m = n - c < kBlock ? n - c : kBlock;
      renderChunk(in + c, out + c, plan, Chunk{pitch.ring(), first + c, m, clock + static_cast<uint32_t>(c)});
    }
  }

  // Test hooks. leadGain is the lead's gain at the end of the last block. activeVoices counts the
  // voices that ticked in it. lagSamples and driftCents read a copy's current lag and pitch drift.
  float leadGain() const { return lead_; }
  uint8_t activeVoices() const { return active_; }
  float lagSamples(size_t v) const { return lag_[v].samples(); }
  float driftCents(size_t v) const { return drift_[v].cents(); }

 private:
  using FormantPsolaVoice = gang_detail::FormantPsolaVoice;
  static constexpr size_t kVoices = gang_detail::kVoices;

  // Block-end targets for everything the knobs and the tuning set.
  struct Plan {
    std::array<float, kVoices> voice{};  // fixed gain times fade, per voice
    float lead = 1.0f;
    float wet = 0.0f;  // gang multiplier times the trim
  };

  static Plan makePlan(const GangParams& p) {
    Plan plan;
    if (!p.on) return plan;
    const GangTuning& t = p.tuning;
    const gang_detail::VoicesLaw law = gang_detail::voicesLaw(smooth::clamp01(p.voices), t, p.solo);
    plan.voice = law.weight;
    plan.lead = law.lead;
    plan.wet = law.mul * dbToGainFast(t.trimDb);
    return plan;
  }

  bool settledOff(const GangParams& p) const {
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
    for (size_t v = 0; v < kVoices; ++v) retire(v);
    hp_.clear();
    dc_.clear();
    contour_.reset();
    epochs_.reset();
    active_ = 0;
    tiltSet_ = false;
    breathSet_ = false;
  }

  void retire(size_t v) {
    voice_[v].reset();
    tilt_[v].clear();
    lag_[v].restart();
    drift_[v].reset();
    breath_[v].env = 0.0f;
    breath_[v].amt = 0.0f;
    breath_[v].hp.clear();
    breathTarget_[v] = 0.0f;
    levelMul_[v] = 1.0f;
    live_[v] = false;
  }

  // The low cut on the gang sum. The setter runs on the first block after a reset and when the
  // tuning moved.
  void setLowCut(const GangTuning& t) {
    if (hpSet_ && t.hpHz == hpHz_) return;
    hp_.setHighPass(t.hpHz, kButterworthQ);
    hpHz_ = t.hpHz;
    hpSet_ = true;
  }

  // One high shelf per voice, set on the first block after a reset and when the tuning moved.
  void setTilts(const GangTuning& t) {
    for (size_t v = 0; v < kVoices; ++v) {
      if (tiltSet_ && t.tiltDb[v] == tiltDb_[v] && t.tiltHz == tiltHz_) continue;
      tilt_[v].setHighShelf(t.tiltHz, t.tiltDb[v]);
      tiltDb_[v] = t.tiltDb[v];
    }
    tiltHz_ = t.tiltHz;
    tiltSet_ = true;
  }

  // Copy v's lag target in samples. The scale stretches the delay and the ceiling caps it.
  static float lagTarget(const GangTuning& t, size_t v) {
    const float lagMs = t.delayMs[v] * t.lagScale;
    return (lagMs < t.maxLagMs ? lagMs : t.maxLagMs) * gang_detail::kSmpPerMs;
  }

  // The breath high-pass of each copy, set on the first block after a reset and when breathHz moved.
  void setBreathFilters(const GangTuning& t) {
    if (breathSet_ && t.breathHz == breathHz_) return;
    for (size_t v = 0; v < kVoices; ++v) breath_[v].hp.setHighPass(t.breathHz, kButterworthQ);
    breathHz_ = t.breathHz;
    breathSet_ = true;
  }

  // What every voice of one block reads.
  struct Block {
    const GangTuning& t;
    gang_detail::BlockView view;
    float period;
    float corr;        // Autotune's correction in octaves
    float width;
    float scatterSmp;  // timing scatter in samples
    float levelDb;     // level scatter, clamped to its range
    float slew;        // one-pole coefficient of the slow walks for this block
    int n;
    const gang_detail::EpochTrack* epochs;  // null cuts on the period grid
  };

  // The shared pulse marks of this block, or null when the grid cuts. Off drops the marks, so a
  // later on starts a fresh search.
  const gang_detail::EpochTrack* prepareEpochs(const GangTuning& t, const PitchFx& pitch, float period, int n) {
    if (!t.epochAlign) {
      epochs_.reset();
      return nullptr;
    }
    const float p = period > 0.0f ? clampf(period, gang_detail::kMinPeriod, gang_detail::kMaxPeriod) : 0.0f;
    epochs_.advance(pitch.shadow(), pitch.head(), n, p, t.epochSearch);
    return &epochs_;
  }

  // Per block: filters, the contour track and the pulse marks, then each live voice. first is the
  // ring index of the block's first sample.
  void prepareBlock(const GangParams& p, const PitchFx& pitch, const Plan& plan, int first, int n) {
    const GangTuning& t = p.tuning;
    setLowCut(t);
    setTilts(t);
    setBreathFilters(t);
    const PitchResult& pr = pitch.pitch();
    const float slew = 1.0f - expf(-static_cast<float>(n) / (kSampleRate * gang_detail::kDriftSlewS));
    contour_.step(pr.period, pr.voiced, slew);
    const gang_detail::EpochTrack* epochs = prepareEpochs(t, pitch, pr.period, n);
    const Block b{t,
                  {pitch.ring(), first, n, dbToGainFast(2.0f * t.gapFloorDb)},
                  pr.period,
                  pitch.correctionSemis() * (1.0f / 12.0f),
                  smooth::clamp01(p.width),
                  (t.scatterMs > 0.0f ? t.scatterMs : 0.0f) * gang_detail::kSmpPerMs,
                  clampf(t.levelScatterDb, 0.0f, gang_detail::kMaxLevelScatterDb),
                  slew,
                  n,
                  epochs};
    active_ = 0;
    for (size_t v = 0; v < kVoices; ++v) prepareVoice(v, plan, b);
  }

  // Copy v's lag, loudness, breath, pitch ratio and playback rate. A voice whose gain sits at 0
  // across the block is cleared and does not tick.
  void prepareVoice(size_t v, const Plan& plan, const Block& b) {
    if (gain_[v] <= 0.0f && plan.voice[v] <= 0.0f) {
      if (live_[v]) retire(v);
      return;
    }
    const GangTuning& t = b.t;
    live_[v] = true;
    ++active_;
    const bool roll = b.levelDb > 0.0f;
    if (lag_[v].settle(b.view, lagTarget(t, v), b.scatterSmp, b.period, roll)) voice_[v].markReset();
    levelMul_[v] = roll ? dbToGainFast(lag_[v].levelUnit() * b.levelDb) : 1.0f;
    breathTarget_[v] = clampf(t.breath[v], 0.0f, gang_detail::kMaxBreath);
    const float drift = drift_[v].step(driftRng_[v], t.driftCents, t.driftHoldS, b.slew, b.n);
    const float st = gang_detail::widthSt(t.pitchSt[v], b.width);
    const float oct = st * (1.0f / 12.0f) + drift * (1.0f / 1200.0f) + b.corr;
    voice_[v].prepare(b.period, exp2Fast(oct + contourOctaves(v, t)), exp2Fast(t.formantSt[v] * (1.0f / 12.0f)),
                      lag_[v].samples());
    voice_[v].setEpochs(b.epochs);
  }

  // How far copy v's pitch moves from the lead's, in octaves: (contour - 1) of Adam's deviation.
  // The copy then sits contour times as far from his mean as he does. Contour 1 adds exactly 0.
  float contourOctaves(size_t v, const GangTuning& t) const {
    const float c = clampf(t.contour[v], gang_detail::kMinContour, gang_detail::kMaxContour);
    return c == 1.0f ? 0.0f : (c - 1.0f) * contour_.deviation();
  }

  // One chunk of the call: kBlock samples at most.
  struct Chunk {
    const VoiceRing& ring;
    int first;       // ring index of the chunk's first sample
    int n;
    uint32_t clock;  // epoch clock of that sample
  };

  // Adds voice v into gang. The voice's tone shelf runs first, then its gain ramps to the
  // block-end target. The shelf runs on a local copy so its state stays in registers.
  void mixVoice(size_t v, const Chunk& k, float target, float* gang) {
    std::array<float, kBlock> raw{};
    voice_[v].render(k.ring, k.first, k.n, raw.data(), k.clock);
    if (breath_[v].amt > 0.0f || breathTarget_[v] > 0.0f) {
      mixBreathy(v, raw.data(), k.n, target, gang);
      return;
    }
    Biquad tilt = tilt_[v];
    const float step = (target - gain_[v]) / static_cast<float>(k.n);
    float g = gain_[v];
    for (int i = 0; i < k.n; ++i) {
      g += step;
      gang[i] += g * tilt.run(raw[static_cast<size_t>(i)]);
    }
    tilt_[v] = tilt;
  }

  // The same mix with breath. The noise follows the level of the copy's own tone, so it sounds under
  // the copy and falls silent with it, and the breath amount ramps to its target across the chunk. A
  // breath that has ramped to 0 clears its filter and follower, so it starts clean next time.
  void mixBreathy(size_t v, const float* raw, int n, float target, float* gang) {
    Breath b = breath_[v];
    Biquad tilt = tilt_[v];
    const float inv = 1.0f / static_cast<float>(n);
    const float step = (target - gain_[v]) * inv;
    const float bStep = (breathTarget_[v] - b.amt) * inv;
    float g = gain_[v];
    for (int i = 0; i < n; ++i) {
      const float x = tilt.run(raw[i]);
      const float a = fabsf(x);
      b.env += (a > b.env ? gang_detail::kBreathAttack : gang_detail::kBreathRelease) * (a - b.env);
      b.amt += bStep;
      g += step;
      gang[i] += g * (x + b.amt * b.env * b.hp.run(b.rng.next()));
    }
    b.amt = breathTarget_[v];
    if (b.amt == 0.0f) {
      b.env = 0.0f;
      b.hp.clear();
    } else if (b.env < kEnvFloor) {
      b.env = 0.0f;  // a long silence would otherwise decay the follower into subnormals
    }
    breath_[v] = b;
    tilt_[v] = tilt;
  }

  // The low cut on the gang sum, in place. The filter runs on a local copy so its state stays in
  // registers.
  void filterGang(float* gang, int n) {
    Biquad hp = hp_;
    for (int i = 0; i < n; ++i) gang[i] = hp.run(gang[i]);
    hp_ = hp;
  }

  // Lead and gang ramp linearly from the last chunk's values to the plan. A voice's gain target is
  // its plan gain times the loudness its last gap drew, which is exactly 1 with level scatter off.
  void renderChunk(const float* in, float* out, const Plan& plan, const Chunk& k) {
    std::array<float, kBlock> gang{};
    for (size_t v = 0; v < kVoices; ++v) {
      if (live_[v]) mixVoice(v, k, plan.voice[v] * levelMul_[v], gang.data());
    }
    filterGang(gang.data(), k.n);
    const float inv = 1.0f / static_cast<float>(k.n);
    for (int i = 0; i < k.n; ++i) {
      const float f = static_cast<float>(i + 1) * inv;
      const float wet = dc_.run(gang[static_cast<size_t>(i)]);
      out[i] = (lead_ + (plan.lead - lead_) * f) * in[i] + (wet_ + (plan.wet - wet_) * f) * wet;
    }
    for (size_t v = 0; v < kVoices; ++v) gain_[v] = plan.voice[v] * levelMul_[v];
    lead_ = plan.lead;
    wet_ = plan.wet;
  }

  // The follower is far under any audible level below this and snaps to 0.
  static constexpr float kEnvFloor = 1e-9f;

  // A copy's breath noise: its high-pass, its random stream, the follower on its own tone and the
  // breath amount at the end of the last chunk.
  struct Breath {
    Biquad hp;
    gang_detail::XorShift rng;
    float env = 0.0f;
    float amt = 0.0f;
  };

  std::array<FormantPsolaVoice, kVoices> voice_{};
  Biquad hp_;
  std::array<Biquad, kVoices> tilt_{};
  std::array<float, kVoices> tiltDb_{};
  std::array<float, kVoices> gain_{};
  std::array<bool, kVoices> live_{};
  std::array<gang_detail::CopyLag, kVoices> lag_{};
  std::array<gang_detail::PitchDrift, kVoices> drift_{};
  std::array<gang_detail::XorShift, kVoices> driftRng_{};
  std::array<Breath, kVoices> breath_{};
  std::array<float, kVoices> breathTarget_{};
  std::array<float, kVoices> levelMul_ = {1.0f, 1.0f, 1.0f, 1.0f};
  gang_detail::ContourTrack contour_;
  gang_detail::EpochTrack epochs_;
  DcBlocker dc_;
  float lead_ = 1.0f;
  float wet_ = 0.0f;
  float hpHz_ = 0.0f;
  float tiltHz_ = 0.0f;
  float breathHz_ = 0.0f;
  uint8_t active_ = 0;
  bool hpSet_ = false;
  bool tiltSet_ = false;
  bool breathSet_ = false;
  bool idle_ = true;
};

}  // namespace cv
