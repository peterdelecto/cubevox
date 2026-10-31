#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
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

  // Adds n samples of the voice into out. first is the ring index of the block's first sample;
  // the voice reads nothing newer than it. Launches are placed sample by sample, then each grain
  // plays through the block on its own.
  void render(const VoiceRing& ring, int first, int n, float* out) {
    if (!voiced_) return;
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
  }

  // Unsets the lag and keeps the random stream.
  void restart() {
    set_ = false;
    landed_ = false;
    quiet_ = 0;
  }

  float samples() const { return lag_; }

  // Per block. base is the target in samples before scatter. True when the lag landed on a new
  // value, so the voice restarts its marks there.
  bool settle(const BlockView& b, float base, float scatterSmp, float period) {
    const float want = clampLag(base + offset_ * scatterSmp, period);
    if (!set_) return land(base, want);
    const bool moved = base != base_;
    if (!moved && scatterSmp <= 0.0f) {
      quiet_ = 0;
      landed_ = false;
      return false;
    }
    const bool here = b.quietAt(clampLag(lag_, period));
    if (!here) landed_ = false;
    const bool pending = moved || !landed_;
    const bool calm = here && (!pending || b.quietAt(want));
    quiet_ = calm ? quiet_ + 1 : 0;
    return quiet_ >= kQuietBlocks && pending && land(base, want);
  }

 private:
  bool land(float base, float want) {
    lag_ = want;
    base_ = base;
    set_ = true;
    landed_ = true;
    quiet_ = 0;
    offset_ = rng_.next();
    return true;
  }

  XorShift rng_;
  float offset_ = 0.0f;  // the next scatter draw, -1..1
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
    for (int c = 0; c < n; c += kBlock) {
      const int m = n - c < kBlock ? n - c : kBlock;
      renderChunk(in + c, out + c, m, first + c, plan, pitch.ring());
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
    active_ = 0;
    tiltSet_ = false;
  }

  void retire(size_t v) {
    voice_[v].reset();
    tilt_[v].clear();
    lag_[v].restart();
    drift_[v].reset();
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

  // Per block: filters, then each live voice's lag, pitch ratio and playback rate. A voice whose
  // gain sits at 0 across the block is cleared and does not tick. first is the ring index of the
  // block's first sample.
  void prepareBlock(const GangParams& p, const PitchFx& pitch, const Plan& plan, int first, int n) {
    const GangTuning& t = p.tuning;
    setLowCut(t);
    setTilts(t);
    const float period = pitch.pitch().period;
    const float corr = pitch.correctionSemis() * (1.0f / 12.0f);
    const float w = smooth::clamp01(p.width);
    const float scatter = (t.scatterMs > 0.0f ? t.scatterMs : 0.0f) * gang_detail::kSmpPerMs;
    const float slew = 1.0f - expf(-static_cast<float>(n) / (kSampleRate * gang_detail::kDriftSlewS));
    const gang_detail::BlockView view{pitch.ring(), first, n, dbToGainFast(2.0f * t.gapFloorDb)};
    active_ = 0;
    for (size_t v = 0; v < kVoices; ++v) {
      if (gain_[v] <= 0.0f && plan.voice[v] <= 0.0f) {
        if (live_[v]) retire(v);
        continue;
      }
      live_[v] = true;
      ++active_;
      if (lag_[v].settle(view, lagTarget(t, v), scatter, period)) voice_[v].markReset();
      const float drift = drift_[v].step(driftRng_[v], t.driftCents, t.driftHoldS, slew, n);
      const float st = gang_detail::widthSt(t.pitchSt[v], w);
      voice_[v].prepare(period, exp2Fast(st * (1.0f / 12.0f) + drift * (1.0f / 1200.0f) + corr),
                        exp2Fast(t.formantSt[v] * (1.0f / 12.0f)), lag_[v].samples());
    }
  }

  // Adds voice v into gang. The voice's tone shelf runs first, then its gain ramps to the
  // block-end target. The shelf runs on a local copy so its state stays in registers.
  void mixVoice(size_t v, const VoiceRing& ring, int first, int n, float target, float* gang) {
    std::array<float, kBlock> raw{};
    voice_[v].render(ring, first, n, raw.data());
    Biquad tilt = tilt_[v];
    const float step = (target - gain_[v]) / static_cast<float>(n);
    float g = gain_[v];
    for (int i = 0; i < n; ++i) {
      g += step;
      gang[i] += g * tilt.run(raw[static_cast<size_t>(i)]);
    }
    tilt_[v] = tilt;
  }

  // The low cut on the gang sum, in place. The filter runs on a local copy so its state stays in
  // registers.
  void filterGang(float* gang, int n) {
    Biquad hp = hp_;
    for (int i = 0; i < n; ++i) gang[i] = hp.run(gang[i]);
    hp_ = hp;
  }

  // first is the ring index of the chunk's first sample. Lead and gang ramp linearly from the last
  // chunk's values to the plan.
  void renderChunk(const float* in, float* out, int n, int first, const Plan& plan, const VoiceRing& ring) {
    std::array<float, kBlock> gang{};
    for (size_t v = 0; v < kVoices; ++v) {
      if (live_[v]) mixVoice(v, ring, first, n, plan.voice[v], gang.data());
    }
    filterGang(gang.data(), n);
    const float inv = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      const float f = static_cast<float>(i + 1) * inv;
      const float wet = dc_.run(gang[static_cast<size_t>(i)]);
      out[i] = (lead_ + (plan.lead - lead_) * f) * in[i] + (wet_ + (plan.wet - wet_) * f) * wet;
    }
    for (size_t v = 0; v < kVoices; ++v) gain_[v] = plan.voice[v];
    lead_ = plan.lead;
    wet_ = plan.wet;
  }

  std::array<FormantPsolaVoice, kVoices> voice_{};
  Biquad hp_;
  std::array<Biquad, kVoices> tilt_{};
  std::array<float, kVoices> tiltDb_{};
  std::array<float, kVoices> gain_{};
  std::array<bool, kVoices> live_{};
  std::array<gang_detail::CopyLag, kVoices> lag_{};
  std::array<gang_detail::PitchDrift, kVoices> drift_{};
  std::array<gang_detail::XorShift, kVoices> driftRng_{};
  DcBlocker dc_;
  float lead_ = 1.0f;
  float wet_ = 0.0f;
  float hpHz_ = 0.0f;
  float tiltHz_ = 0.0f;
  uint8_t active_ = 0;
  bool hpSet_ = false;
  bool tiltSet_ = false;
  bool idle_ = true;
};

}  // namespace cv
