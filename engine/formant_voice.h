#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/pitch.h"
#include "engine/psola.h"

// The grain voice that the Mob card's copies and the Formant card share. It reads the pitch stage's
// input ring and cuts grains on the tracker's period grid, with a pitch ratio and a grain playback
// rate of its own. The owner of the ring, the tracker and the mix (PitchFx, Crowd) drives it.

namespace cv {
namespace formant_detail {

// The periods a voice plays are the ones the tracker reports.
constexpr float kMinPeriod = static_cast<float>(PitchTracker::kMinPeriod);
constexpr float kMaxPeriod = static_cast<float>(PitchTracker::kMaxPeriod);

// The range of a grain's playback rate, 2^(formant / 12).
constexpr float kMinRate = 0.5f;
constexpr float kMaxRate = 2.0f;

// The output lags the input by the read delay. A grain centred on a mark needs one and a half
// periods of lookahead, so the delay never drops below that plus a pad, and never rises past what
// leaves room in the ring for the grain's reach and drift.
constexpr float kFloorPeriods = 1.5f;
constexpr float kFloorPad = 4.0f;
constexpr float kDelayCeil = static_cast<float>(kVoiceRingLen - kCrowdGrainReach);

constexpr int kMaxMarkSteps = 128;

static_assert(kDelayCeil > kFloorPeriods * kMaxPeriod + kFloorPad, "no room for a read delay");

// Clamps a lag to what the voice can play at this pitch period.
inline float clampLag(float lag, float period) {
  const float p = PitchTracker::clampPeriod(period);
  return clampf(lag, kFloorPeriods * p + kFloorPad, kDelayCeil);
}

// The grain window, 1 - cos(2 pi k / 1024) for k in 0..1024, made at compile time as
// 2 sin^2(pi k / 1024) from a double series, so it sits in flash and a launch runs no libm. A grain
// keeps a 32-bit turn counter. Its top kHannBits pick an entry and the rest interpolate to the next,
// so the window stays smooth. A plain table read steps the window by up to 0.006, which raised
// knob_step's curvature for VOICES from x0.92 to x1.13.
constexpr int kHannBits = 10;
constexpr int kHannShift = 32 - kHannBits;
constexpr uint32_t kHannFracMask = (1u << kHannShift) - 1u;
constexpr float kHannFracScale = 1.0f / static_cast<float>(1u << kHannShift);

// sin x for x in [0, pi / 2], to double rounding.
constexpr double hannSine(double x) {
  double term = x;
  double sum = x;
  for (int k = 1; k < 14; ++k) {
    term *= -x * x / static_cast<double>((2 * k) * (2 * k + 1));
    sum += term;
  }
  return sum;
}

using HannTable = std::array<float, (size_t{1} << kHannBits) + 1>;

constexpr HannTable makeHannTable() {
  constexpr double kPiD = 3.14159265358979323846;
  constexpr size_t kLen = size_t{1} << kHannBits;
  HannTable t{};
  for (size_t k = 0; k <= kLen; ++k) {
    const size_t fold = k <= kLen / 2 ? k : kLen - k;
    const double s = hannSine(kPiD * static_cast<double>(fold) / static_cast<double>(kLen));
    t[k] = static_cast<float>(2.0 * s * s);
  }
  return t;
}

inline constexpr HannTable kHannTable = makeHannTable();

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
    lastPeriod_ = 0.0f;
  }

  // The next prepare() puts the marks at its lag. Grains in flight finish where they are.
  void markReset() { fresh_ = true; }

  // True until a voiced prepare() has put the marks down. That call's first sample launches a grain.
  bool starting() const { return fresh_; }

  // The grain playback rate the last prepare() took, for the tests.
  float rate() const { return rate_; }

  // On, a grain keeps the gain it launched with, so a step in rate or ratio reaches only the grains
  // that launch after it. Off, every grain in flight takes the newest gain, as the Mob copies'
  // slow glides have it. The Mob card runs it off, and crowd_test's pinned hashes and the Mob
  // renders hold that sound.
  void latchGain(bool on) { latch_ = on; }

  // Per call. period 0 silences the voice. delay is the wanted lag of the output behind the
  // input in samples. The marks sit one period closer than that, because a grain starts one
  // period before its mark. n is the call's length, written to the ring ahead of the read.
  void prepare(float period, float ratio, float rate, float delay, int n) {
    voiced_ = period > 0.0f;
    if (!voiced_) {
      count_ = 0;
      lastPeriod_ = 0.0f;
      return;
    }
    // The tracker publishes a period every fourth block, so most calls repeat the last one's work.
    if (!fresh_ && period == lastPeriod_ && ratio == lastRatio_ && rate == lastRate_ && delay == lastDelay_ &&
        n == lastN_) {
      return;
    }
    lastPeriod_ = period;
    lastRatio_ = ratio;
    lastRate_ = rate;
    lastDelay_ = delay;
    lastN_ = n;
    p_ = PitchTracker::clampPeriod(period);
    rate_ = clampf(rate, kMinRate, kMaxRate);
    step_ = static_cast<int>(rate_ * static_cast<float>(1 << kFracBits) + 0.5f);
    const float r = clampf(ratio, kMinPitchRatio, kMaxPitchRatio);
    outPeriod_ = p_ / r;
    setGrain(r);
    turn_ = kTurn32 / dur_;
    turnStep_ = static_cast<uint32_t>(turn_);
    drift_ = (1.0f - rate_) * dur_;
    setReadSpan(n);
    ref_ = clampLag(delay, p_) - p_;
    if (fresh_) {
      markDelay_ = ref_;
      countdown_ = 0.0f;
      fresh_ = false;
    }
  }

  VoiceGrid grid() const { return {markDelay_, countdown_}; }

  // Takes another voice's grid, so the two launch on the same samples. Call it after prepare().
  void adoptGrid(const VoiceGrid& g) {
    markDelay_ = g.markDelay;
    countdown_ = g.countdown;
  }

  // Adds n samples of the voice into out. first is the ring index of the block's first sample;
  // the voice reads nothing newer than it. The countdown jumps from launch to launch, then each
  // grain plays through the block on its own. outPeriod_ holds for the block, so one clamp at its
  // start does what a clamp on every sample would. zeroBefore is how many samples just ahead of the
  // block the ring holds as exact zeros, if the caller knows. A grain whose reads all fall inside
  // that run would add exact zeros, so it skips its taps and moves on as if it had played.
  void render(const VoiceRing& ring, int first, int n, float* out, int zeroBefore = 0) {
    if (!voiced_) return;
    for (size_t k = 0; k < count_; ++k) grains_[k].start = 0;
    if (countdown_ > outPeriod_) countdown_ = outPeriod_;
    for (int i = 0;;) {
      const int k = countdown_ > 1.0f ? static_cast<int>(ceilf(countdown_)) : 1;
      if (k > n - i) {
        stepMarks(n - i);
        countdown_ -= static_cast<float>(n - i);
        break;
      }
      stepMarks(k);
      countdown_ -= static_cast<float>(k);
      i += k;
      const int head = wrapOnce<kVoiceRingLen>(first + i - 1);
      if (count_ == 0) prefill(head, -countdown_, i - 1);
      launch(head, -countdown_, i - 1);
      countdown_ += outPeriod_;
    }
    size_t kept = 0;
    for (size_t k = 0; k < count_; ++k) {
      if (play(ring.data(), grains_[k], out, n, first, zeroBefore)) grains_[kept++] = grains_[k];
    }
    count_ = kept;
  }

 private:
  // The read position is a fixed-point ring index, so a step of rate samples is one integer add.
  // The ring length shifted up must stay inside an int.
  static constexpr int kFracBits = 16;
  static constexpr int kFracMask = (1 << kFracBits) - 1;
  static constexpr int kRingFix = kVoiceRingLen << kFracBits;
  static constexpr int kLastFix = (kVoiceRingLen - 1) << kFracBits;  // a read here pairs the last sample with the first
  static constexpr float kFracScale = 1.0f / static_cast<float>(1 << kFracBits);

  static_assert(kVoiceRingLen < (1 << (31 - kFracBits)) - 4, "ring index overflows the fixed-point position");

  struct Grain {
    uint32_t turn = 0;      // window phase, a full turn over the grain
    uint32_t turnStep = 0;  // turn advance per output sample
    float age = 0.0f;
    float dur = 0.0f;
    float amp = 0.0f;  // the voice's gain when the grain launched
    int pos = 0;    // ring index of the next read, in 1 / 2^kFracBits samples
    int step = 0;   // pos advance per output sample
    int start = 0;  // first sample of the block it sounds in
  };

  // k samples of advanceMarks(). A stretch that stays two samples clear of ref_ + p_ cannot trip
  // the realign test, so it goes in one add, and whole-sample adds round as single steps do. The
  // samples around a wrap step one at a time, so the marks match a walk sample by sample bit for bit.
  void stepMarks(int k) {
    while (k > 0) {
      const float room = ref_ + p_ - markDelay_;
      const int clear = markDelay_ >= ref_ && room > 3.0f ? static_cast<int>(room) - 2 : 0;
      if (clear > 0) {
        const int run = clear < k ? clear : k;
        markDelay_ += static_cast<float>(run);
        k -= run;
      } else {
        advanceMarks();
        --k;
      }
    }
  }

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

  // The grain's length and gain. A grain lasts two periods of input at the playback rate, but never
  // less than two output periods: a Hann window shorter than two spacings leaves the window sum
  // dipping between grains, which pulses breaths and sibilants at the sung pitch. The gain sets the
  // window sum to one.
  void setGrain(float ratio) {
    const float natural = 2.0f * p_ / rate_;
    const float floor = 2.0f * outPeriod_;
    if (natural >= floor) {
      dur_ = natural;
      amp_ = 0.5f * rate_ / ratio;
    } else {
      dur_ = floor;
      amp_ = 0.5f;
    }
  }

  // The span a launch may read. A grain's delay moves by 1 - rate per sample, so both ends leave
  // room for that drift. The far end also leaves the call's samples, or kBlock when the call is
  // shorter, so a grain stays on valid samples through any later call no longer than that.
  void setReadSpan(int n) {
    const float written = static_cast<float>(n > kBlock ? n : kBlock);
    lo_ = kMinReadDelay + (drift_ < 0.0f ? -drift_ : 0.0f);
    hi_ = kMaxReadDelay - written - (drift_ > 0.0f ? drift_ : 0.0f);
  }

  // lead is how far past the ideal launch instant this tick already is. The grain starts one
  // period before its mark.
  void launch(int head, float lead, int at) {
    float centre = markDelay_;
    if (centre - ref_ > 0.5f * p_) centre -= p_;
    start(head, clampf(centre + p_ - rate_ * lead, lo_, hi_), lead, at);
  }

  // A voice with no grain in flight also lays the grain that would have started one output period
  // earlier, cut a period further back, so the overlap sums to one from its first sample and the
  // copy comes in without a dip. A grain needs a sample left to play.
  void prefill(int head, float lead, int at) {
    const float age = lead + outPeriod_;
    if (age >= dur_ - 1.0f) return;
    float centre = markDelay_;
    if (centre - ref_ > 0.5f * p_) centre -= p_;
    start(head, clampf(centre + 2.0f * p_ - rate_ * age, lo_, hi_), age, at);
  }

  // A grain that reads `delay` behind head now, age samples into its window.
  void start(int head, float delay, float age, int at) {
    const float pos = static_cast<float>(head) - delay;
    const float fl = floorf(pos);
    Grain& g = nextSlot();
    g.pos = (wrapOnce<kVoiceRingLen>(static_cast<int>(fl)) << kFracBits) +
            static_cast<int>((pos - fl) * static_cast<float>(1 << kFracBits));
    g.step = step_;
    g.age = age;
    g.dur = dur_;
    g.amp = amp_;
    g.start = at;
    g.turn = static_cast<uint32_t>(age * turn_);
    g.turnStep = turnStep_;
  }

  // A grain's read state while it plays, held in locals. tap() gives one output sample from ring
  // samples i0 and i1 (the sample after i0) and steps the window and the read position.
  struct Cursor {
    uint32_t turn;
    uint32_t turnStep;
    int pos;
    int step;
    float amp;

    CV_INLINE float tap(const float* ring, int i0, int i1) {
      const float x0 = ring[i0];
      const float t = static_cast<float>(pos & kFracMask) * kFracScale;
      const uint32_t k = turn >> kHannShift;
      const float f = static_cast<float>(turn & kHannFracMask) * kHannFracScale;
      const float w = kHannTable[k] + f * (kHannTable[k + 1] - kHannTable[k]);
      turn += turnStep;
      pos += step;
      return amp * w * (x0 + t * (ring[i1] - x0));
    }
  };

  // True when the m samples the grain plays this block read only the run of zeroBefore zeros that ends
  // just ahead of the block, and none of the block's own samples. The read delay moves by 1 - rate per
  // sample, so its ends bound it; the bounds leave a sample either side for the interpolation.
  bool readsZeros(const Grain& g, int m, int n, int first, int zeroBefore) const {
    int head = first + g.start;
    if (head >= kVoiceRingLen) head -= kVoiceRingLen;
    int delay = head - (g.pos >> kFracBits);
    if (delay < 0) delay += kVoiceRingLen;
    const float drift = static_cast<float>(m - 1) * (1.0f - static_cast<float>(g.step) * (1.0f / static_cast<float>(1 << kFracBits)));
    const float newest = static_cast<float>(delay - 1) + (drift < 0.0f ? drift : 0.0f);
    const float oldest = static_cast<float>(delay) + (drift > 0.0f ? drift : 0.0f);
    return newest >= static_cast<float>(n + 4) && oldest + 2.0f <= static_cast<float>(zeroBefore);
  }

  // Plays the grain's share of this block into out and returns true if it lives on into the next.
  // The last table entry closes the turn, so the window read needs no wrap. The run splits where
  // the read reaches the ring's last sample, worked out once per stretch, so the plain stretches
  // carry no wrap test. Only a read on that last sample pairs it with the ring's first.
  bool play(const float* CV_RESTRICT ring, Grain& g, float* CV_RESTRICT out, int n, int first, int zeroBefore) const {
    const int room = n - g.start;
    const float left = g.dur - g.age;
    const int remain = left > 0.0f ? static_cast<int>(ceilf(left)) : 0;
    const int m = remain < room ? remain : room;
    if (zeroBefore > 0 && m > 0 && readsZeros(g, m, n, first, zeroBefore)) {
      g.turn += g.turnStep * static_cast<uint32_t>(m);
      g.pos += g.step * m;
      if (g.pos >= kRingFix) g.pos -= kRingFix;
      g.age += static_cast<float>(m);
      return remain > room;
    }
    Cursor c{g.turn, g.turnStep, g.pos, g.step, latch_ ? g.amp : amp_};
    float* o = out + g.start;
    for (int j = 0; j < m;) {
      const int plain = c.pos < kLastFix ? (kLastFix - c.pos + c.step - 1) / c.step : 0;
      for (const int end = plain < m - j ? j + plain : m; j < end; ++j) {
        const int i0 = c.pos >> kFracBits;
        o[j] += c.tap(ring, i0, i0 + 1);
      }
      if (c.pos >= kRingFix) c.pos -= kRingFix;
      if (j < m && c.pos >= kLastFix) {
        o[j++] += c.tap(ring, kVoiceRingLen - 1, 0);
        if (c.pos >= kRingFix) c.pos -= kRingFix;
      }
    }
    g.turn = c.turn;
    g.pos = c.pos;
    g.age += static_cast<float>(m);
    return remain > room;
  }

  std::array<Grain, kMaxGrains> grains_{};
  size_t count_ = 0;
  int step_ = 1 << kFracBits;
  float lastPeriod_ = 0.0f;  // the arguments of the last prepare() that did the work, 0 when it has none
  float lastRatio_ = 0.0f;
  float lastRate_ = 0.0f;
  float lastDelay_ = 0.0f;
  int lastN_ = 0;
  float p_ = kMinPeriod;
  float rate_ = 1.0f;
  float outPeriod_ = kMinPeriod;
  float dur_ = 2.0f * kMinPeriod;
  float turn_ = kTurn32 / (2.0f * kMinPeriod);  // window turn per sample of grain, as a float
  uint32_t turnStep_ = static_cast<uint32_t>(kTurn32 / (2.0f * kMinPeriod));
  float amp_ = 0.5f;  // half the grain gain, rate / ratio, so the window needs no 0.5
  float drift_ = 0.0f;
  float lo_ = kMinReadDelay;  // the delays a launch may read, set per call
  float hi_ = kMinReadDelay;
  float ref_ = 0.0f;
  float markDelay_ = 0.0f;
  float countdown_ = 0.0f;
  bool voiced_ = false;
  bool fresh_ = true;
  bool latch_ = false;
};

}  // namespace formant_detail
}  // namespace cv
