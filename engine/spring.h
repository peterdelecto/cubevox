#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/chasm.h"
#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/smooth.h"

// Spring reverb: three springs of different lengths in parallel, cheap enough for the H7. The card
// reads "Spring Reverb"; the design spec calls it SPRING B.
//
// Each spring is one feedback loop: delay -> treble loss -> bass cut -> diffusers a, b ->
// disperser -> back into its own delay, plus the input. A spring's taps repeat springMs apart.
// The dispersers are stretched allpasses (a + z^-K) / (1 + a z^-K). With chirpSign +1 the
// coefficient is positive, lows arrive first and each echo sweeps up; with -1 it is negative,
// highs arrive first and each echo sweeps down. The dispersers sit inside the loops, so every
// echo picks up more sweep than the last: the drip. The three spacings share no short common
// multiple, so the springs' chirps stack into a splash rather than a periodic boing. Light diffusers
// keep each echo distinct. The in-loop treble loss and bass cut are light, so the flutter
// survives in the tail; a one-pole low-pass on the summed return takes off the metallic top.
// DECAY sets the feedback g per pass. The input scales by sqrt((1 - g^2) / g) so the tail's level
// holds as DECAY moves: sqrt(1 - g^2) holds the echo sum's energy, and 1 / sqrt(g) offsets the
// first echo leaving at g, less what the loop filters take each pass. Dwell, a tuning value with
// no knob, drives the Chasm soft clip ahead of the loops. Output is wet only, at wetDb.

namespace cv {

struct SpringTuning {
  float springMs1 = 37.0f, springMs2 = 45.0f, springMs3 = 56.0f;  // tap-to-tap echo spacing
  float timeLo = 0.55f, timeHi = 0.86f;    // feedback per pass at DECAY 0 / 1
  float diffuse = 0.30f;                   // diffuser coefficient
  int chirpSections = 32;                  // disperser sections per spring
  float chirpA = 0.80f;                    // disperser coefficient size; higher = longer sweep
  float chirpSign = 1.0f;                  // +1 sweeps up (lows first), -1 sweeps down
  float trebleLossHz = 9000.0f;            // in-loop one-pole low-pass
  float bassCutHz = 200.0f;                // in-loop one-pole high-pass
  float returnLpHz = 5000.0f;              // one-pole low-pass on the summed return
  float wobbleDepth = 8.0f;                // samples
  float wobbleRateHz = 3.0f;
  float inputTrim = 0.5f;
  float dwellDrive = 4.0f, dwellComp = 0.8f;  // input drive at dwell 1; level comp exponent
  float wetDb = 6.0f;                      // trim; level rule on the wet alone at the box's TIME and dwell
};

struct SpringParams {
  float decay = 0.50f;  // set by TIME through applyKnobs
  float dwell = 0.20f;  // input drive into the soft clip
  SpringTuning tuning;
};

namespace spring_detail {

constexpr float kMsToSamples = static_cast<float>(kSampleRate) / 1000.0f;

constexpr int kDiff1A = 139, kDiff1B = 207;  // 2.9 / 4.3 ms
constexpr int kDiff2A = 178, kDiff2B = 254;  // 3.7 / 5.3 ms
constexpr int kDiff3A = 163, kDiff3B = 233;  // 3.4 / 4.9 ms
constexpr int kStretch = 6;                  // K; the sweep sits below fs / 2K = 4 kHz
constexpr int kMaxSections = 32;
constexpr float kMaxSpringMs = 80.0f;
constexpr float kMaxWobble = 32.0f;          // samples
constexpr int kDlyLen = static_cast<int>(kMaxSpringMs * kMsToSamples) + 4;
constexpr int kMinDly = 16;                  // shortest line, samples
constexpr float kTimeMin = 0.10f;            // feedback floor; the input scale divides by it
constexpr float kTimeMax = 0.97f;            // stability ceiling
constexpr float kPhasePerHz = 4294967296.0f / static_cast<float>(kSampleRate);

// Schroeder allpass with a runtime coefficient.
template <int N>
struct Diffuser {
  std::array<float, N> buf{};
  int pos = 0;

  float run(float in, float k) {
    const float out = buf[static_cast<size_t>(pos)] + k * in;
    buf[static_cast<size_t>(pos)] = in - k * out;
    if (++pos >= N) pos = 0;
    return out;
  }

  void clear() {
    buf.fill(0.0f);
    pos = 0;
  }
};

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
  Spring() {
    for (int i = 0; i < 257; ++i)
      sine_[static_cast<size_t>(i)] = sinf(static_cast<float>(i) * (kTwoPi / 256.0f));
    reset();
  }

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
    decay_ = dwell_ = 0.0f;
    valid_ = false;
  }

  // Wet only; the caller mixes. n <= kBlock.
  void process(const float* in, float* wet, int n, const SpringParams& p) {
    const SpringTuning& t = p.tuning;
    const float decayTarget = smooth::clamp01(p.decay);
    const float dwellTarget = smooth::clamp01(p.dwell);
    if (!valid_) {
      decay_ = decayTarget;
      dwell_ = dwellTarget;
      valid_ = true;
    }
    const Loop loop = prepareLoop(t);

    // Smoothed values are evaluated at the block edges and interpolated.
    float decayEnd = decay_, dwellEnd = dwell_;
    for (int i = 0; i < n; ++i) {
      decayEnd = smooth::step(decayEnd, decayTarget, smooth::kSmoothCoef);
      dwellEnd = smooth::step(dwellEnd, dwellTarget, smooth::kSmoothCoef);
    }
    const Input in0 = inputAt(t, loop, decay_, dwell_);
    const Input in1 = inputAt(t, loop, decayEnd, dwellEnd);
    const float inv = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
      const float f = static_cast<float>(i + 1) * inv;
      const float g = in0.feedback + (in1.feedback - in0.feedback) * f;
      const float drive = in0.drive + (in1.drive - in0.drive) * f;
      const float gain = in0.gain + (in1.gain - in0.gain) * f;
      wet[i] = tick(softClipHeadroom(in[i] * drive, kChasmClipHeadroom) * gain, g, loop);
    }
    decay_ = decayEnd;
    dwell_ = dwellEnd;
  }

 private:
  using Line = chasm_detail::Delay<spring_detail::kDlyLen>;

  // Everything the loops take from the tuning, once per block.
  struct Loop {
    float timeLo, timeHi, k, chirpA, lpF, hpF, retF, depth, adv1, adv2, adv3, wetGain;
    uint32_t lfoStep;
    int sections;
  };

  // Feedback per pass, input drive and input gain at one decay and dwell. The input gain carries
  // the drive's level compensation, the trim and the sqrt((1 - g^2) / g) scale.
  struct Input {
    float feedback, drive, gain;
  };

  // Reads the tuning for this block. Disperser sections that come back into use start clean.
  Loop prepareLoop(const SpringTuning& t) {
    using namespace spring_detail;
    Loop l;
    l.timeHi = clampf(t.timeHi, kTimeMin, kTimeMax);
    l.timeLo = clampf(t.timeLo, kTimeMin, l.timeHi);
    l.k = clampf(t.diffuse, 0.0f, 0.9f);
    const float a = clampf(fabsf(t.chirpA), 0.0f, 0.95f);
    l.chirpA = t.chirpSign < 0.0f ? -a : a;
    l.sections = t.chirpSections < 0 ? 0 : t.chirpSections;
    if (l.sections > kMaxSections) l.sections = kMaxSections;
    if (l.sections > sections_) disp_.clearFrom(sections_);
    sections_ = l.sections;
    l.lpF = onePoleCoef(t.trebleLossHz);
    l.hpF = onePoleCoef(t.bassCutHz);
    l.retF = onePoleCoef(t.returnLpHz);
    l.depth = clampf(t.wobbleDepth, 0.0f, kMaxWobble);
    l.lfoStep = static_cast<uint32_t>(clampf(t.wobbleRateHz, 0.0f, 20.0f) * kPhasePerHz);
    l.adv1 = advance(t.springMs1, kDiff1A + kDiff1B, l.depth);
    l.adv2 = advance(t.springMs2, kDiff2A + kDiff2B, l.depth);
    l.adv3 = advance(t.springMs3, kDiff3A + kDiff3B, l.depth);
    l.wetGain = powf(10.0f, t.wetDb / 20.0f);
    return l;
  }

  static Input inputAt(const SpringTuning& t, const Loop& l, float decay, float dwell) {
    const float logDrive = logf(t.dwellDrive < 1.0f ? 1.0f : t.dwellDrive);
    const float g = l.timeLo + decay * (l.timeHi - l.timeLo);
    const float comp = expf(-t.dwellComp * logDrive * dwell);
    return {g, expf(logDrive * dwell), comp * t.inputTrim * sqrtf((1.0f - g * g) / g)};
  }

  // One sample of the three loops for the driven input x at feedback g.
  float tick(float x, float g, const Loop& l) {
    lfoAcc_ += l.lfoStep;
    const float out1 = filter(dly1_.read(l.adv1 + lfo(0u) * l.depth) * g, lp1_, hp1_, l.lpF, l.hpF);
    const float out2 = filter(dly2_.read(l.adv2 + lfo(85u) * l.depth) * g, lp2_, hp2_, l.lpF, l.hpF);
    const float out3 = filter(dly3_.read(l.adv3 + lfo(171u) * l.depth) * g, lp3_, hp3_, l.lpF, l.hpF);
    float tap1 = d1b_.run(d1a_.run(out1, l.k), l.k);
    float tap2 = d2b_.run(d2a_.run(out2, l.k), l.k);
    float tap3 = d3b_.run(d3a_.run(out3, l.k), l.k);
    disp_.run(tap1, tap2, tap3, l.sections, l.chirpA);
    dly1_.write(tap1 + x);
    dly2_.write(tap2 + x);
    dly3_.write(tap3 + x);
    ret_ += (tap1 + tap2 + tap3 - ret_) * l.retF;
    return ret_ * l.wetGain;
  }

  // Read advance that makes the spring's delay plus its diffusers springMs long,
  // with room left for the wobble to shorten it further.
  static float advance(float springMs, int diffusers, float depth) {
    using namespace spring_detail;
    const float len = clampf(springMs * kMsToSamples - static_cast<float>(diffusers),
                             static_cast<float>(kMinDly) + depth, static_cast<float>(kDlyLen));
    return static_cast<float>(kDlyLen) - len;
  }

  // One-pole treble loss, then one-pole bass cut.
  static float filter(float in, float& lp, float& hp, float lpF, float hpF) {
    lp += (in - lp) * lpF;
    hp += (lp - hp) * hpF;
    return lp - hp;
  }

  // Unipolar sine 0..1; phase8 offsets in 1/256 turns.
  float lfo(uint32_t phase8) const {
    const uint32_t i0 = ((lfoAcc_ >> 24) + phase8) & 0xFFu;
    const float fr = static_cast<float>(lfoAcc_ & 0x00FFFFFFu) * (1.0f / 16777216.0f);
    const float s = sine_[i0] + fr * (sine_[i0 + 1u] - sine_[i0]);
    return s * 0.5f + 0.5f;
  }

  spring_detail::Diffuser<spring_detail::kDiff1A> d1a_;
  spring_detail::Diffuser<spring_detail::kDiff1B> d1b_;
  spring_detail::Diffuser<spring_detail::kDiff2A> d2a_;
  spring_detail::Diffuser<spring_detail::kDiff2B> d2b_;
  spring_detail::Diffuser<spring_detail::kDiff3A> d3a_;
  spring_detail::Diffuser<spring_detail::kDiff3B> d3b_;
  spring_detail::Dispersers disp_;
  Line dly1_;
  Line dly2_;
  Line dly3_;
  std::array<float, 257> sine_{};
  float lp1_ = 0.0f, lp2_ = 0.0f, lp3_ = 0.0f, hp1_ = 0.0f, hp2_ = 0.0f, hp3_ = 0.0f;
  float ret_ = 0.0f;  // return low-pass state
  uint32_t lfoAcc_ = 0u;
  int sections_ = 0;
  float decay_ = 0.0f, dwell_ = 0.0f;
  bool valid_ = false;
};

}  // namespace cv
