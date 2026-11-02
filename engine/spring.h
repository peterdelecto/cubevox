#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/reverb_parts.h"
#include "engine/smooth.h"

// Spring reverb: three springs of different lengths in parallel, cheap enough for the H7. The card
// reads "Spring Reverb".
//
// Each spring is one feedback loop: delay -> treble loss -> bass cut -> diffusers a, b ->
// disperser -> back into its own delay, plus the input. A spring's taps repeat springMs apart.
// The dispersers are stretched allpasses (a + z^-K) / (1 + a z^-K) with a positive coefficient, so
// lows arrive first and each echo sweeps up. They sit inside the loops, so every echo picks up more
// sweep than the last. That growing sweep is the chirp. The three spacings share no short common
// multiple, so the springs' chirps stack into a splash rather than a periodic boing. Light diffusers
// keep each echo distinct. The in-loop treble loss and bass cut are light, so the flutter
// survives in the tail, and a one-pole low-pass on the summed return takes off the metallic top.
// TIME sets the feedback g per pass. The input scales by sqrt((1 - g^2) / g) so the tail's level
// holds as Time moves. The sqrt(1 - g^2) term holds the echo sum's energy, and 1 / sqrt(g) offsets
// the first echo leaving at g, less what the loop filters take each pass. Dwell, a tuning value with
// no knob, drives the soft clip ahead of the loops. The loops are linear, so wetDb scales their input
// and the output needs no gain. Output is wet only.

namespace cv {

namespace spring_detail {

constexpr float kMsToSamples = static_cast<float>(kSampleRate) / 1000.0f;

constexpr int kDiff1A = 139, kDiff1B = 207;  // 2.9 / 4.3 ms
constexpr int kDiff2A = 178, kDiff2B = 254;  // 3.7 / 5.3 ms
constexpr int kDiff3A = 163, kDiff3B = 233;  // 3.4 / 4.9 ms
constexpr int kStretch = 6;                  // K; the sweep sits below fs / 2K = 4 kHz
constexpr int kMaxSections = 32;
constexpr float kMaxWobble = 32.0f;          // samples
constexpr float kMaxWobbleHz = 10.0f;
constexpr int kMinDly = 16;                  // shortest read, samples
constexpr float kFeedbackMin = 0.10f;        // feedback floor; the input scale divides by it
constexpr float kFeedbackMax = 0.97f;        // stability ceiling
constexpr float kMaxWetDb = 24.0f;           // wetDb clamps to plus and minus this
constexpr float kMaxDwellDrive = 64.0f;      // dwellDrive clamps to 1..this
constexpr float kMaxDwellComp = 2.0f;        // dwellComp clamps to 0..this

// The wobble is evaluated at the block edges and interpolated linearly. The sine's curvature bounds
// the read position's error at depth / 2 * (2 pi rate)^2 * T^2 / 8 samples for a block of T seconds.
constexpr float kMaxWobbleErr = 0.05f;  // samples
constexpr float wobbleError(float depth, float rateHz) {
  const float w = kTwoPi * rateHz;
  const float t = static_cast<float>(kBlock) / static_cast<float>(kSampleRate);
  return 0.5f * depth * w * w * t * t / 8.0f;
}
static_assert(wobbleError(kMaxWobble, kMaxWobbleHz) <= kMaxWobbleErr,
              "the widest, fastest wobble must stay within the interpolation's error limit");

// Default tap-to-tap echo spacing of each spring, ms.
constexpr float kDefaultMs1 = 37.0f, kDefaultMs2 = 45.0f, kDefaultMs3 = 56.0f;

// Each line holds its spring's default read, which is the spacing less the two diffusers, plus this
// margin. A spacing tuned longer than the line clamps to it.
constexpr int kLineMargin = 16;
constexpr int lineLength(float springMs, int diffusers) {
  return static_cast<int>(springMs * kMsToSamples) - diffusers + kLineMargin;
}
constexpr int kDly1 = lineLength(kDefaultMs1, kDiff1A + kDiff1B);
constexpr int kDly2 = lineLength(kDefaultMs2, kDiff2A + kDiff2B);
constexpr int kDly3 = lineLength(kDefaultMs3, kDiff3A + kDiff3B);

// Longest spacing each spring takes before its line clamps it, ms.
constexpr float kMaxMs1 = static_cast<float>(kDly1 + kDiff1A + kDiff1B) / kMsToSamples;
constexpr float kMaxMs2 = static_cast<float>(kDly2 + kDiff2A + kDiff2B) / kMsToSamples;
constexpr float kMaxMs3 = static_cast<float>(kDly3 + kDiff3A + kDiff3B) / kMsToSamples;

}  // namespace spring_detail

struct SpringTuning {
  float springMs1 = spring_detail::kDefaultMs1, springMs2 = spring_detail::kDefaultMs2,
        springMs3 = spring_detail::kDefaultMs3;  // tap-to-tap echo spacing
  float feedbackLo = 0.55f, feedbackHi = 0.86f;  // feedback per pass at TIME 0 / 1
  float diffuse = 0.30f;                   // diffuser coefficient
  int chirpSections = 32;                  // disperser sections per spring
  float chirpA = 0.80f;                    // disperser coefficient; higher = longer sweep
  float trebleLossHz = 9000.0f;            // in-loop one-pole low-pass
  float bassCutHz = 200.0f;                // in-loop one-pole high-pass
  float returnLpHz = 5000.0f;              // one-pole low-pass on the summed return
  float wobbleDepth = 8.0f;                // samples
  float wobbleRateHz = 3.0f;
  float dwell = 0.20f;                     // input drive into the soft clip, 0..1
  float dwellDrive = 4.0f, dwellComp = 0.8f;  // input drive at dwell 1; level comp exponent
  float wetDb = 0.29f;                     // gain ahead of the loops; level rule on the wet alone at the box's TIME and dwell, the mean of Adam's clips
  float restLevel = 1.0e-6f;               // -120 dBFS; a silent input lets the springs rest once every loop sits below it, 0 never rests
};

static_assert(std::is_trivially_copyable<SpringTuning>::value && sizeof(SpringTuning) == 18 * sizeof(float),
              "the engine compares the tuning bytes to see whether it changed");

struct SpringParams {
  float decay = 0.50f;  // set by TIME through applyKnobs
  SpringTuning tuning;
};

namespace spring_detail {

// Three disperser chains, one per spring, each up to kMaxSections stretched allpasses K samples
// long. The chains advance together, so their states sit side by side in one buffer.
struct Dispersers {
  std::array<float, kMaxSections * kStretch * 3> buf{};
  int pos = 0;

  // One sample through the first `sections` sections of every chain. The loop takes two sections
  // per pass, so each section's output feeds the next without a register copy.
  void run(float& x1, float& x2, float& x3, int sections, float a) {
    constexpr int kStep = kStretch * 3;
    float* z = buf.data() + pos * 3;
    float y1 = x1, y2 = x2, y3 = x3;
    int u = 0;
    for (; u + 2 <= sections; u += 2, z += 2 * kStep) {
      float* w = z + kStep;
      const float o1 = z[0] + a * y1;
      z[0] = y1 - a * o1;
      y1 = w[0] + a * o1;
      w[0] = o1 - a * y1;
      const float o2 = z[1] + a * y2;
      z[1] = y2 - a * o2;
      y2 = w[1] + a * o2;
      w[1] = o2 - a * y2;
      const float o3 = z[2] + a * y3;
      z[2] = y3 - a * o3;
      y3 = w[2] + a * o3;
      w[2] = o3 - a * y3;
    }
    if (u < sections) {
      const float o1 = z[0] + a * y1;
      const float o2 = z[1] + a * y2;
      const float o3 = z[2] + a * y3;
      z[0] = y1 - a * o1;
      z[1] = y2 - a * o2;
      z[2] = y3 - a * o3;
      y1 = o1;
      y2 = o2;
      y3 = o3;
    }
    x1 = y1;
    x2 = y2;
    x3 = y3;
    if (++pos >= kStretch) pos = 0;
  }

  // Sections from..end held stale state while unused.
  void clearFrom(int from) {
    for (size_t i = static_cast<size_t>(from * kStretch * 3); i < buf.size(); ++i) buf[i] = 0.0f;
  }

  void clear() {
    buf.fill(0.0f);
    pos = 0;
  }
};

}  // namespace spring_detail

class Spring {
 public:
  Spring() { reset(); }

  void reset() {
    d1a_.clear();
    d1b_.clear();
    d2a_.clear();
    d2b_.clear();
    d3a_.clear();
    d3b_.clear();
    disp_.clear();
    dly1_.clear();
    dly2_.clear();
    dly3_.clear();
    lp1_ = lp2_ = lp3_ = hp1_ = hp2_ = hp3_ = ret_ = 0.0f;
    lfoAcc_ = 0u;
    sections_ = 0;
    decay_ = 0.0f;
    valid_ = false;
    built_ = false;
    inputValid_ = false;
    resting_ = false;
    scanSpan_ = 0;
    scanPos_ = 0;
    scanPeak_ = 0.0f;
  }

  // True while the springs rest, which is test information only.
  bool resting() const { return resting_; }

  // Wet only; the caller mixes.
  //
  // With a silent input the springs run on until every loop has decayed below restLevel, which a
  // slow scan of their state confirms. Then they rest, output zeros and hold their state, which sits
  // under restLevel. The first block with sound ends the rest and the loops carry on from that state.
  // A tail that is still audible keeps the springs running, whatever the MIX.
  void process(const float* in, float* wet, int n, const SpringParams& p) {
    const float decayTarget = smooth::clamp01(p.decay);
    if (!valid_) {
      decay_ = decayTarget;
      valid_ = true;
    }
    refresh(p.tuning);
    const float restLevel = p.tuning.restLevel;
    const bool silent = below(in, n, restLevel);
    if (resting_) {
      if (silent) {
        rest(wet, n, decayTarget);
        return;
      }
      resting_ = false;
    }
    run(in, wet, n, decayTarget);
    if (!silent) {
      scanSpan_ = 0;
      scanPos_ = 0;
      scanPeak_ = 0.0f;
    } else if (scanStep(restLevel)) {
      resting_ = true;
    }
  }

 private:
  // A resting block outputs zeros. The wobble and TIME's smoother keep their pace, so the loops pick
  // up in step when sound returns.
  void rest(float* wet, int n, float decayTarget) {
    for (int i = 0; i < n; ++i) wet[i] = 0.0f;
    lfoAcc_ += static_cast<uint32_t>(n) * loop_.lfoStep;
    if (decay_ != decayTarget) {
      for (int i = 0; i < n; ++i) decay_ = smooth::step(decay_, decayTarget, smooth::kSmoothCoef);
    }
  }

  void run(const float* in, float* wet, int n, float decayTarget) {
    const Loop& loop = loop_;

    // The feedback and input gain at TIME's smoothed value come from the last block when it ended
    // there. While TIME is still the smoother holds its target, so the block runs at one value.
    // While it moves, the two are evaluated at the block edges and interpolated.
    if (!inputValid_ || inputDecay_ != decay_) {
      input_ = inputAt(loop, decay_);
      inputDecay_ = decay_;
      inputValid_ = true;
    }
    const Input in0 = input_;
    const float inv = 1.0f / static_cast<float>(n);
    const Wobble wob = wobbleFor(loop, n);
    if (decay_ == decayTarget) {
      for (int i = 0; i < n; ++i) {
        const float f = static_cast<float>(i + 1) * inv;
        wet[i] = tick(softClipHeadroom(in[i] * loop.drive, kReverbClipHeadroom) * in0.gain, in0.feedback, f, wob, loop);
      }
      return;
    }
    float decayEnd = decay_;
    for (int i = 0; i < n; ++i) decayEnd = smooth::step(decayEnd, decayTarget, smooth::kSmoothCoef);
    const Input in1 = inputAt(loop, decayEnd);
    for (int i = 0; i < n; ++i) {
      const float f = static_cast<float>(i + 1) * inv;
      const float g = in0.feedback + (in1.feedback - in0.feedback) * f;
      const float gain = in0.gain + (in1.gain - in0.gain) * f;
      wet[i] = tick(softClipHeadroom(in[i] * loop.drive, kReverbClipHeadroom) * gain, g, f, wob, loop);
    }
    decay_ = decayEnd;
    input_ = in1;
    inputDecay_ = decayEnd;
  }

  // Every sample of the block is under level in magnitude. A NaN is not.
  static bool below(const float* in, int n, float level) {
    for (int i = 0; i < n; ++i)
      if (!(fabsf(in[i]) < level)) return false;
    return true;
  }

  // The scan covers the delay lines, the diffusers and the dispersers, kScanChunk floats a call. It
  // counts a full pass as quiet when none of that state and none of the filter states reached level.
  // Any block with sound restarts the pass, since the loops fill again.
  static constexpr int kScanChunk = 256;
  static constexpr int kScanSpans = 10;

  bool scanStep(float level) {
    using namespace spring_detail;
    struct Span {
      const float* data;
      int len;
    };
    const Span spans[kScanSpans] = {
        {dly1_.buf.data(), kDly1}, {dly2_.buf.data(), kDly2}, {dly3_.buf.data(), kDly3},
        {disp_.buf.data(), static_cast<int>(disp_.buf.size())},
        {d1a_.buf.data(), kDiff1A}, {d1b_.buf.data(), kDiff1B}, {d2a_.buf.data(), kDiff2A},
        {d2b_.buf.data(), kDiff2B}, {d3a_.buf.data(), kDiff3A}, {d3b_.buf.data(), kDiff3B}};
    int budget = kScanChunk;
    while (budget > 0 && scanSpan_ < kScanSpans) {
      const Span& s = spans[scanSpan_];
      const int take = budget < s.len - scanPos_ ? budget : s.len - scanPos_;
      for (int j = 0; j < take; ++j) scanPeak_ = fmaxf(scanPeak_, fabsf(s.data[scanPos_ + j]));
      scanPos_ += take;
      budget -= take;
      if (scanPos_ == s.len) {
        ++scanSpan_;
        scanPos_ = 0;
      }
    }
    if (scanSpan_ < kScanSpans) return false;
    float filters = fabsf(ret_);
    for (const float v : {lp1_, lp2_, lp3_, hp1_, hp2_, hp3_}) filters = fmaxf(filters, fabsf(v));
    const bool quiet = scanPeak_ < level && filters < level;
    scanSpan_ = 0;
    scanPeak_ = 0.0f;
    return quiet;
  }

  // Everything the loops take from the tuning, once per block.
  struct Loop {
    float feedbackLo, feedbackHi, k, chirpA, lpF, hpF, retF, depth, adv1, adv2, adv3;
    float drive, compWet;
    uint32_t lfoStep;
    int sections;
  };

  // Each spring's read position at the block's start and its change to the block's end.
  struct Wobble {
    float base1, base2, base3, rise1, rise2, rise3;
  };

  // The three wobble LFOs, evaluated at the block edges. Sample i of n reads at the fraction
  // (i + 1) / n of the way, so the last sample lands on the end value. Advances the LFO by the block.
  Wobble wobbleFor(const Loop& l, int n) {
    const uint32_t end = lfoAcc_ + static_cast<uint32_t>(n) * l.lfoStep;
    const auto edge = [&](uint32_t phase8, float adv, float& base, float& rise) {
      const float a = reverb_parts::lfo(lfoAcc_, phase8) * l.depth;
      const float b = reverb_parts::lfo(end, phase8) * l.depth;
      base = adv + a;
      rise = b - a;
    };
    Wobble w;
    edge(0u, l.adv1, w.base1, w.rise1);
    edge(85u, l.adv2, w.base2, w.rise2);
    edge(171u, l.adv3, w.base3, w.rise3);
    lfoAcc_ = end;
    return w;
  }

  // Feedback per pass and input gain at one decay. The input gain carries the drive's level
  // compensation, the wet gain and the sqrt((1 - g^2) / g) scale.
  struct Input {
    float feedback, gain;
  };

  // Reads the tuning for this block. Disperser sections that come back into use start clean.
  Loop prepareLoop(const SpringTuning& t) {
    using namespace spring_detail;
    Loop l;
    l.feedbackHi = clampf(t.feedbackHi, kFeedbackMin, kFeedbackMax);
    l.feedbackLo = clampf(t.feedbackLo, kFeedbackMin, l.feedbackHi);
    l.k = clampf(t.diffuse, 0.0f, 0.9f);
    l.chirpA = clampf(t.chirpA, 0.0f, 0.95f);
    l.sections = t.chirpSections < 0 ? 0 : t.chirpSections;
    if (l.sections > kMaxSections) l.sections = kMaxSections;
    if (l.sections > sections_) disp_.clearFrom(sections_);
    sections_ = l.sections;
    l.lpF = onePoleCoef(t.trebleLossHz);
    l.hpF = onePoleCoef(t.bassCutHz);
    l.retF = onePoleCoef(t.returnLpHz);
    l.depth = clampf(t.wobbleDepth, 0.0f, kMaxWobble);
    l.lfoStep = static_cast<uint32_t>(clampf(t.wobbleRateHz, 0.0f, kMaxWobbleHz) * reverb_parts::kPhasePerHz);
    l.adv1 = advance(t.springMs1, kDiff1A + kDiff1B, kDly1, l.depth);
    l.adv2 = advance(t.springMs2, kDiff2A + kDiff2B, kDly2, l.depth);
    l.adv3 = advance(t.springMs3, kDiff3A + kDiff3B, kDly3, l.depth);
    const float wetGain = powf(10.0f, clampf(t.wetDb, -kMaxWetDb, kMaxWetDb) / 20.0f);
    const float dwell = smooth::clamp01(t.dwell);
    const float logDrive = logf(clampf(t.dwellDrive, 1.0f, kMaxDwellDrive));
    l.drive = expf(logDrive * dwell);
    const float comp = expf(-clampf(t.dwellComp, 0.0f, kMaxDwellComp) * logDrive * dwell);
    l.compWet = comp * wetGain;
    return l;
  }

  // Rebuilds the loop terms only when the tuning differs from the last block's, which on the box is
  // the first block alone.
  void refresh(const SpringTuning& t) {
    if (built_ && std::memcmp(&t, &tuning_, sizeof t) == 0) return;
    loop_ = prepareLoop(t);
    tuning_ = t;
    built_ = true;
    inputValid_ = false;
  }

  static Input inputAt(const Loop& l, float decay) {
    const float g = l.feedbackLo + decay * (l.feedbackHi - l.feedbackLo);
    return {g, l.compWet * sqrtf((1.0f - g * g) / g)};
  }

  // One sample of the three loops for the driven input x at feedback g, at fraction f of the way
  // through the block's wobble.
  CV_INLINE float tick(float x, float g, float f, const Wobble& w, const Loop& l) {
    const float out1 = filter(dly1_.read(w.base1 + w.rise1 * f) * g, lp1_, hp1_, l.lpF, l.hpF);
    const float out2 = filter(dly2_.read(w.base2 + w.rise2 * f) * g, lp2_, hp2_, l.lpF, l.hpF);
    const float out3 = filter(dly3_.read(w.base3 + w.rise3 * f) * g, lp3_, hp3_, l.lpF, l.hpF);
    float tap1 = d1b_.run(d1a_.run(out1, l.k), l.k);
    float tap2 = d2b_.run(d2a_.run(out2, l.k), l.k);
    float tap3 = d3b_.run(d3a_.run(out3, l.k), l.k);
    disp_.run(tap1, tap2, tap3, l.sections, l.chirpA);
    dly1_.write(tap1 + x);
    dly2_.write(tap2 + x);
    dly3_.write(tap3 + x);
    ret_ += (tap1 + tap2 + tap3 - ret_) * l.retF;
    return ret_;
  }

  // Read advance that makes the spring's delay plus its diffusers springMs long, with room left for
  // the wobble to shorten it further. A longer spacing clamps to the line of lineLen samples.
  static float advance(float springMs, int diffusers, int lineLen, float depth) {
    using namespace spring_detail;
    const float len = clampf(springMs * kMsToSamples - static_cast<float>(diffusers),
                             static_cast<float>(kMinDly) + depth, static_cast<float>(lineLen));
    return static_cast<float>(lineLen) - len;
  }

  // One-pole treble loss, then one-pole bass cut.
  static float filter(float in, float& lp, float& hp, float lpF, float hpF) {
    lp += (in - lp) * lpF;
    hp += (lp - hp) * hpF;
    return lp - hp;
  }

  reverb_parts::Allpass<spring_detail::kDiff1A> d1a_;
  reverb_parts::Allpass<spring_detail::kDiff1B> d1b_;
  reverb_parts::Allpass<spring_detail::kDiff2A> d2a_;
  reverb_parts::Allpass<spring_detail::kDiff2B> d2b_;
  reverb_parts::Allpass<spring_detail::kDiff3A> d3a_;
  reverb_parts::Allpass<spring_detail::kDiff3B> d3b_;
  spring_detail::Dispersers disp_;
  reverb_parts::Delay<spring_detail::kDly1> dly1_;
  reverb_parts::Delay<spring_detail::kDly2> dly2_;
  reverb_parts::Delay<spring_detail::kDly3> dly3_;
  float lp1_ = 0.0f, lp2_ = 0.0f, lp3_ = 0.0f, hp1_ = 0.0f, hp2_ = 0.0f, hp3_ = 0.0f;
  float ret_ = 0.0f;  // return low-pass state
  uint32_t lfoAcc_ = 0u;
  int sections_ = 0;
  float decay_ = 0.0f;
  SpringTuning tuning_;     // the tuning loop_ was built from
  Loop loop_{};
  Input input_{};           // the input terms at inputDecay_
  float inputDecay_ = 0.0f;
  float scanPeak_ = 0.0f;   // largest magnitude the current scan pass has seen
  int scanSpan_ = 0;        // the state array the scan is in
  int scanPos_ = 0;         // and where in it
  bool valid_ = false;
  bool built_ = false;
  bool inputValid_ = false;
  bool resting_ = false;
};

}  // namespace cv
