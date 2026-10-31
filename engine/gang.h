#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/chasm.h"
#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/pitch_fx.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// The Gang card adds up to four copies of the voice, each a different person standing a step back
// from the mic. Each copy sits a few semitones off the lead, has its own throat (a grain playback
// rate) and its own tone shelf, wobbles slowly in pitch, and lags the lead by its own delay. The
// gang as a whole has less bass and top than the lead. One small room sits on the gang. The copies
// never read a key. They read the pitch period and Autotune's correction from PitchFx and cut
// their grains from its input ring.
//
// VOICES fades the copies in one at a time, then fades the lead out, so the top of the knob is the
// gang alone. Total power is unity at every position. DISTANCE moves the copies' delay, bass cut,
// top cut and room send together.

namespace cv {

struct GangTuning {
  std::array<float, 4> pitchSt = {-2.85f, 2.15f, -4.75f, 4.25f};  // pitch ratio 2^(st / 12) against the lead
  std::array<float, 4> formantSt = {-2.0f, 1.5f, -3.5f, 3.0f};     // grain playback rate 2^(st / 12)
  std::array<float, 4> tiltDb = {-3.0f, 3.0f, -4.0f, 5.0f};        // high shelf gain above tiltHz
  std::array<float, 4> delayMs = {30.0f, 40.0f, 50.0f, 54.0f};     // behind the lead, before the DISTANCE scale
  std::array<float, 4> lfoHz = {0.23f, 0.31f, 0.41f, 0.53f};
  float tiltHz = 2500.0f;
  float lfoCents = 12.0f;      // peak wobble on each voice's pitch
  float voiceDb = -9.0f;       // one voice against the lead at unity
  float trimDb = 3.1f;         // level rule residue
  float leadFadeFrom = 0.8f;   // VOICES position where the lead starts to leave
  float holdFrom = 0.75f;      // part of each voice's fifth spent fading; the rest holds
  float hpNearHz = 120.0f, hpFarHz = 250.0f;
  float shelfHz = 6000.0f, shelfNearDb = -1.0f, shelfFarDb = -5.0f;
  float roomNearDb = -14.0f, roomFarDb = -3.0f;
  float roomLpHz = 4000.0f;    // tail darkness
  float roomFeedback = 0.6f;   // 35 ms loop, about 0.45 s to -60 dB
};

struct GangParams {
  bool on = false;
  float distance = 0.5f;  // knob A, 0..1
  float voices = 0.5f;    // knob B, 0..1
  GangTuning tuning;
};

namespace gang_detail {

constexpr size_t kVoices = 4;
constexpr float kSmpPerMs = kSampleRate / 1000.0f;

// DISTANCE scales every delayMs from kTimingNear at 0 to kTimingFar at 1.
constexpr float kTimingNear = 0.8f;
constexpr float kTimingFar = 1.3f;

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
constexpr float kLfoMaxHz = 20.0f;
constexpr float kTurn = 4294967296.0f;

// Samples in a whole number of milliseconds.
constexpr int ms(int v) { return v * kSampleRate / 1000; }

static_assert(kDelayCeil > kFloorPeriods * kMaxPeriod + kFloorPad, "no room for a read delay");

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
    ref_ = clampf(delay, kFloorPeriods * p_ + kFloorPad, kDelayCeil) - p_;
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
  static constexpr int kFracBits = 18;
  static constexpr int kFracMask = (1 << kFracBits) - 1;
  static constexpr int kRingFix = kVoiceRingLen << kFracBits;
  static constexpr float kFracScale = 1.0f / static_cast<float>(1 << kFracBits);

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

// Six early reflections on one delay line, then a 35 ms feedback loop with a lowpass inside it.
// The reflections feed the loop through two allpasses.
class GangRoom {
 public:
  void reset() {
    early_.fill(0.0f);
    pos_ = 0;
    ap1_.clear();
    ap2_.clear();
    loop_.clear();
    lp_.clear();
  }

  // Per block, since the lowpass coefficient calls expf.
  void setTail(float lpHz, float feedback) {
    if (!lpSet_ || lpHz != lpHz_) {
      lp_.set(lpHz);
      lpHz_ = lpHz;
      lpSet_ = true;
    }
    feedback_ = clampf(feedback, 0.0f, kMaxFeedback);
  }

  // One sample of the room alone, scaled so white noise comes out at its own power. The send then
  // reads as the room's level against the gang.
  float run(float x) {
    early_[static_cast<size_t>(pos_)] = x;
    float er = 0.0f;
    for (size_t k = 0; k < kTapGain.size(); ++k) {
      const int at = pos_ - kTapSamples[k];
      er += kTapGain[k] * early_[static_cast<size_t>(at < 0 ? at + kEarlyLen : at)];
    }
    pos_ = pos_ + 1 == kEarlyLen ? 0 : pos_ + 1;
    const float tail = lp_.lp(loop_.buf[static_cast<size_t>(loop_.pos)]);
    loop_.write(ap2_.run(ap1_.run(er)) + feedback_ * tail);
    return kNorm * (er + tail);
  }

 private:
  static constexpr std::array<int, 6> kTapSamples = {ms(11), ms(17), ms(23), ms(31), ms(37), ms(43)};
  static constexpr std::array<float, 6> kTapGain = {0.9f, 0.75f, 0.6f, 0.5f, 0.4f, 0.3f};
  static constexpr int kEarlyLen = ms(43) + ms(1);
  static constexpr float kMaxFeedback = 0.9f;
  static constexpr float kNorm = 0.58f;

  std::array<float, kEarlyLen> early_{};
  chasm_detail::Allpass<ms(5)> ap1_;
  chasm_detail::Allpass<ms(7)> ap2_;
  chasm_detail::Delay<ms(35)> loop_;
  OnePole lp_;
  float lpHz_ = 0.0f;
  float feedback_ = 0.0f;
  int pos_ = 0;
  bool lpSet_ = false;
};

// The VOICES law at knob position k. weight[v] is the voice's fixed gain times its fade, lead the
// lead's gain, mul the gang multiplier that carries the top of the knob.
struct VoicesLaw {
  std::array<float, kVoices> weight{};
  float lead = 1.0f;
  float mul = 1.0f;
};

inline VoicesLaw voicesLaw(float k, const GangTuning& t) {
  VoicesLaw law;
  const float g = dbToGainFast(t.voiceDb);
  const float from = clampf(t.leadFadeFrom, 0.05f, 0.99f);
  const float hold = clampf(t.holdFrom, 0.05f, 1.0f);
  const float u = k / (from * 0.25f);
  float sumSq = 0.0f;
  for (size_t v = 0; v < kVoices; ++v) {
    const float x = smooth::clamp01((u - static_cast<float>(v)) / hold);
    law.weight[v] = g * sinf(kHalfPi * x);
    sumSq += law.weight[v] * law.weight[v];
  }
  if (k <= from) {
    law.lead = sqrtf(sumSq < 1.0f ? 1.0f - sumSq : 0.0f);
    return law;
  }

  // Past the fade point every voice holds at g. Gang power runs from four voices up to unity
  // while the lead falls to nothing, and the gang multiplier makes up the difference.
  const float s = smooth::clamp01((k - from) / (1.0f - from));
  const float p4 = 4.0f * g * g;
  const float sn = sinf(kHalfPi * s);
  const float power = s >= 1.0f ? 1.0f : p4 + (1.0f - p4) * sn * sn;
  law.weight.fill(g);
  law.lead = sqrtf(power < 1.0f ? 1.0f - power : 0.0f);
  law.mul = p4 > 1e-6f ? sqrtf(power / p4) : 1.0f;
  return law;
}

}  // namespace gang_detail

class Gang {
 public:
  // The constructor runs reset(), which keeps the object out of .data: the room's 17 KB of zeros
  // would otherwise sit in flash beside the few non-zero defaults.
  Gang() { reset(); }

  void reset() {
    clearDsp();
    gain_.fill(0.0f);
    lead_ = 1.0f;
    wet_ = 0.0f;
    send_ = 0.0f;
    lfoPhase_ = {0u, 0x40000000u, 0x80000000u, 0xC0000000u};
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
    const float d = stepDistance(p.distance, n);
    const Plan plan = makePlan(p, d);
    prepareBlock(p, pitch, plan, d, n);
    const int first = pitch.head() - n;
    for (int c = 0; c < n; c += kBlock) {
      const int m = n - c < kBlock ? n - c : kBlock;
      renderChunk(in + c, out + c, m, first + c, plan, pitch.ring());
    }
  }

  // Test hooks. leadGain is the lead's gain at the end of the last block. activeVoices counts
  // the voices that ticked in it.
  float leadGain() const { return lead_; }
  uint8_t activeVoices() const { return active_; }

 private:
  using FormantPsolaVoice = gang_detail::FormantPsolaVoice;
  static constexpr size_t kVoices = gang_detail::kVoices;

  // Block-end targets for everything the knobs and the tuning set.
  struct Plan {
    std::array<float, kVoices> voice{};  // fixed gain times fade, per voice
    float lead = 1.0f;
    float wet = 0.0f;   // gang multiplier times the trim
    float send = 0.0f;  // room send
  };

  // DISTANCE follows its knob through the 20 ms smoother, one step per sample of the block, so the
  // filters, the delays and the room send glide to a new knob value. The first block after a
  // reset lands on the knob.
  float stepDistance(float knob, int n) {
    const float target = smooth::clamp01(knob);
    if (!distanceSet_) {
      distance_ = target;
      distanceSet_ = true;
    }
    for (int i = 0; i < n; ++i) distance_ = smooth::step(distance_, target, smooth::kSmoothCoef);
    return distance_;
  }

  // d is the smoothed DISTANCE.
  static Plan makePlan(const GangParams& p, float d) {
    Plan plan;
    if (!p.on) return plan;
    const GangTuning& t = p.tuning;
    const gang_detail::VoicesLaw law = gang_detail::voicesLaw(smooth::clamp01(p.voices), t);
    plan.voice = law.weight;
    plan.lead = law.lead;
    plan.wet = law.mul * dbToGainFast(t.trimDb);
    plan.send = dbToGainFast(t.roomNearDb + (t.roomFarDb - t.roomNearDb) * d);
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
    shelf_.clear();
    room_.reset();
    dc_.clear();
    active_ = 0;
    distanceSet_ = false;
    tiltSet_ = false;
  }

  void retire(size_t v) {
    voice_[v].reset();
    tilt_[v].clear();
    live_[v] = false;
  }

  // All four voices share one filter setting and the filters are linear, so one highpass and one
  // shelf on the gang sum equal one pair per voice. The setters run only when DISTANCE or the
  // tuning moved.
  void setFilters(const GangTuning& t, float d) {
    const float hz = t.hpNearHz + (t.hpFarHz - t.hpNearHz) * d;
    const float db = t.shelfNearDb + (t.shelfFarDb - t.shelfNearDb) * d;
    if (filtersSet_ && hz == hpHz_ && db == shelfDb_ && t.shelfHz == shelfHz_) return;
    hp_.setHighPass(hz, kButterworthQ);
    shelf_.setHighShelf(t.shelfHz, db);
    hpHz_ = hz;
    shelfDb_ = db;
    shelfHz_ = t.shelfHz;
    filtersSet_ = true;
  }

  // One high shelf per voice. The setters run on the first block after a reset and when the
  // tuning moved.
  void setTilts(const GangTuning& t) {
    for (size_t v = 0; v < kVoices; ++v) {
      if (tiltSet_ && t.tiltDb[v] == tiltDb_[v] && t.tiltHz == tiltHz_) continue;
      tilt_[v].setHighShelf(t.tiltHz, t.tiltDb[v]);
      tiltDb_[v] = t.tiltDb[v];
    }
    tiltHz_ = t.tiltHz;
    tiltSet_ = true;
  }

  // Sine of voice v's wobble at the block start, then the phase moves on by n samples.
  float stepLfo(size_t v, float hz, int n) {
    const float s = sinTurns(lfoPhase_[v]);
    const uint32_t inc = static_cast<uint32_t>(clampf(hz, 0.0f, gang_detail::kLfoMaxHz) / kSampleRate *
                                                    gang_detail::kTurn + 0.5f);
    lfoPhase_[v] += inc * static_cast<uint32_t>(n);
    return s;
  }

  // Per block: filters, room, wobble, and each live voice's pitch ratio, playback rate and delay.
  // A voice whose gain sits at 0 across the block is cleared and does not tick.
  void prepareBlock(const GangParams& p, const PitchFx& pitch, const Plan& plan, float d, int n) {
    const GangTuning& t = p.tuning;
    setFilters(t, d);
    setTilts(t);
    room_.setTail(t.roomLpHz, t.roomFeedback);
    const float period = pitch.pitch().period;
    const float corr = pitch.correctionSemis() * (1.0f / 12.0f);
    const float timing =
        (gang_detail::kTimingNear + (gang_detail::kTimingFar - gang_detail::kTimingNear) * d) *
        gang_detail::kSmpPerMs;
    active_ = 0;
    for (size_t v = 0; v < kVoices; ++v) {
      const float wobble = t.lfoCents * stepLfo(v, t.lfoHz[v], n);
      if (gain_[v] <= 0.0f && plan.voice[v] <= 0.0f) {
        if (live_[v]) retire(v);
        continue;
      }
      live_[v] = true;
      ++active_;
      voice_[v].prepare(period, exp2Fast(t.pitchSt[v] * (1.0f / 12.0f) + wobble * (1.0f / 1200.0f) + corr),
                        exp2Fast(t.formantSt[v] * (1.0f / 12.0f)), t.delayMs[v] * timing);
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

  // Highpass then shelf on the gang sum, in place. The filters run on local copies so their state
  // stays in registers.
  void filterGang(float* gang, int n) {
    Biquad hp = hp_;
    Biquad shelf = shelf_;
    for (int i = 0; i < n; ++i) gang[i] = shelf.run(hp.run(gang[i]));
    hp_ = hp;
    shelf_ = shelf;
  }

  // first is the ring index of the chunk's first sample. Lead, gang and send ramp linearly from
  // the last chunk's values to the plan.
  void renderChunk(const float* in, float* out, int n, int first, const Plan& plan, const VoiceRing& ring) {
    std::array<float, kBlock> gang{};
    for (size_t v = 0; v < kVoices; ++v) {
      if (live_[v]) mixVoice(v, ring, first, n, plan.voice[v], gang.data());
    }
    filterGang(gang.data(), n);
    const float inv = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      const float f = static_cast<float>(i + 1) * inv;
      const float send = send_ + (plan.send - send_) * f;
      const float wet = dc_.run(gang[static_cast<size_t>(i)] + send * room_.run(gang[static_cast<size_t>(i)]));
      out[i] = (lead_ + (plan.lead - lead_) * f) * in[i] + (wet_ + (plan.wet - wet_) * f) * wet;
    }
    for (size_t v = 0; v < kVoices; ++v) gain_[v] = plan.voice[v];
    lead_ = plan.lead;
    wet_ = plan.wet;
    send_ = plan.send;
  }

  std::array<FormantPsolaVoice, kVoices> voice_{};
  Biquad hp_;
  Biquad shelf_;
  std::array<Biquad, kVoices> tilt_{};
  std::array<float, kVoices> tiltDb_{};
  std::array<float, kVoices> gain_{};
  std::array<bool, kVoices> live_{};
  std::array<uint32_t, kVoices> lfoPhase_ = {0u, 0x40000000u, 0x80000000u, 0xC0000000u};
  gang_detail::GangRoom room_;
  DcBlocker dc_;
  float lead_ = 1.0f;
  float wet_ = 0.0f;
  float send_ = 0.0f;
  float hpHz_ = 0.0f;
  float shelfHz_ = 0.0f;
  float shelfDb_ = 0.0f;
  float tiltHz_ = 0.0f;
  float distance_ = 0.5f;  // DISTANCE after the smoother
  uint8_t active_ = 0;
  bool filtersSet_ = false;
  bool tiltSet_ = false;
  bool distanceSet_ = false;
  bool idle_ = true;
};

}  // namespace cv
