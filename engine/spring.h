#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/chasm.h"
#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/smooth.h"

// Spring reverb: the Chasm loop voiced as a spring, cheap enough for the H7. The card reads
// "Spring Reverb"; the design spec calls it SPRING B.
//
// Loop: dly1 -> filters -> diffusers 1a, 1b -> disperser 1 -> dly2 -> filters
// -> diffusers 2a, 2b -> disperser 2 -> dly1. Each half is one echo, so the
// taps after the dispersers repeat at halfMs1 and halfMs2 apart, a spring's
// flutter. The dispersers are stretched allpasses (a + z^-K) / (1 + a z^-K)
// with a > 0, so lows arrive first and each echo sweeps up. They sit inside
// the loop, so every echo picks up more sweep than the last: the drip.
// Light diffusers keep the echoes distinct. In-loop treble loss and bass cut
// darken and thin each pass. DECAY sets the feedback; the input scales by
// sqrt(1 - g^2) so the tail's level holds as DECAY moves. Dwell, a tuning
// value with no knob, drives the Chasm soft clip ahead of the loop. Output is
// wet only, at wetDb.

namespace cv {

struct SpringTuning {
  float halfMs1 = 33.0f, halfMs2 = 41.0f;  // tap-to-tap echo spacing per loop half
  float timeLo = 0.70f, timeHi = 0.93f;    // feedback per half at DECAY 0 / 1
  float diffuse = 0.30f;                   // diffuser coefficient
  int chirpSections = 24;                  // disperser sections per half
  float chirpA = 0.70f;                    // disperser coefficient; higher = longer sweep
  float trebleLossHz = 6000.0f;
  float bassCutHz = 250.0f;
  float wobbleDepth = 8.0f;                // samples
  float wobbleRateHz = 3.0f;
  float inputTrim = 0.5f;
  float dwellDrive = 8.0f, dwellComp = 0.8f;  // input drive at dwell 1; level comp exponent
  float wetDb = 8.9f;                      // trim; level rule on the wet alone at the box's TIME and dwell
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
constexpr int kStretch = 6;                  // K; the sweep sits below fs / 2K = 4 kHz
constexpr int kMaxSections = 32;
constexpr float kMaxHalfMs = 80.0f;
constexpr float kMaxWobble = 32.0f;          // samples
constexpr int kDlyLen = static_cast<int>(kMaxHalfMs * kMsToSamples) + 4;
constexpr int kMinDly = 16;                  // shortest line, samples
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

// Up to kMaxSections stretched allpasses in series, each K samples long.
struct Disperser {
  std::array<float, kMaxSections * kStretch> buf{};
  int pos = 0;

  // Runs two dispersers in one section loop so the independent chains overlap.
  static void runPair(Disperser& d1, Disperser& d2, float& in1, float& in2, int sections, float a) {
    for (int u = 0; u < sections; ++u) {
      float& z1 = d1.buf[static_cast<size_t>(u * kStretch + d1.pos)];
      float& z2 = d2.buf[static_cast<size_t>(u * kStretch + d2.pos)];
      const float out1 = z1 + a * in1;
      const float out2 = z2 + a * in2;
      z1 = in1 - a * out1;
      z2 = in2 - a * out2;
      in1 = out1;
      in2 = out2;
    }
    if (++d1.pos >= kStretch) d1.pos = 0;
    if (++d2.pos >= kStretch) d2.pos = 0;
  }

  // Sections from..end held stale state while unused.
  void clearFrom(int from) {
    for (size_t i = static_cast<size_t>(from * kStretch); i < buf.size(); ++i) buf[i] = 0.0f;
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
    disp1_.clear();
    disp2_.clear();
    dly1_.clear();
    dly2_.clear();
    lp1_ = lp2_ = hp1_ = hp2_ = 0.0f;
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

  // Everything the loop takes from the tuning, once per block.
  struct Loop {
    float timeLo, timeHi, k, chirpA, lpF, hpF, depth, adv1, adv2, wetGain;
    uint32_t lfoStep;
    int sections;
  };

  // Feedback per half, input drive and input gain at one decay and dwell. The input gain carries
  // the drive's level compensation, the trim and the sqrt(1 - g^2) scale.
  struct Input {
    float feedback, drive, gain;
  };

  // Reads the tuning for this block. Disperser sections that come back into use start clean.
  Loop prepareLoop(const SpringTuning& t) {
    using namespace spring_detail;
    Loop l;
    l.timeHi = clampf(t.timeHi, 0.0f, kTimeMax);
    l.timeLo = clampf(t.timeLo, 0.0f, l.timeHi);
    l.k = clampf(t.diffuse, 0.0f, 0.9f);
    l.chirpA = clampf(t.chirpA, 0.0f, 0.95f);
    l.sections = t.chirpSections < 0 ? 0 : t.chirpSections;
    if (l.sections > kMaxSections) l.sections = kMaxSections;
    if (l.sections > sections_) {
      disp1_.clearFrom(sections_);
      disp2_.clearFrom(sections_);
    }
    sections_ = l.sections;
    l.lpF = onePoleCoef(t.trebleLossHz);
    l.hpF = onePoleCoef(t.bassCutHz);
    l.depth = clampf(t.wobbleDepth, 0.0f, kMaxWobble);
    l.lfoStep = static_cast<uint32_t>(clampf(t.wobbleRateHz, 0.0f, 20.0f) * kPhasePerHz);
    l.adv1 = advance(t.halfMs1, kDiff1A + kDiff1B, l.depth);
    l.adv2 = advance(t.halfMs2, kDiff2A + kDiff2B, l.depth);
    l.wetGain = powf(10.0f, t.wetDb / 20.0f);
    return l;
  }

  static Input inputAt(const SpringTuning& t, const Loop& l, float decay, float dwell) {
    const float logDrive = logf(t.dwellDrive < 1.0f ? 1.0f : t.dwellDrive);
    const float g = l.timeLo + decay * (l.timeHi - l.timeLo);
    const float comp = expf(-t.dwellComp * logDrive * dwell);
    return {g, expf(logDrive * dwell), comp * t.inputTrim * sqrtf(1.0f - g * g)};
  }

  // One sample of the loop for the driven input x at feedback g.
  float tick(float x, float g, const Loop& l) {
    using spring_detail::Disperser;
    lfoAcc_ += l.lfoStep;
    // Both lines are read before either is written.
    const float out1 = filter(dly1_.read(l.adv1 + lfo(0u) * l.depth) * g, lp1_, hp1_, l.lpF, l.hpF);
    const float out2 = filter(dly2_.read(l.adv2 + lfo(64u) * l.depth) * g, lp2_, hp2_, l.lpF, l.hpF);
    float tap1 = d1b_.run(d1a_.run(out1, l.k), l.k);
    float tap2 = d2b_.run(d2a_.run(out2, l.k), l.k);
    Disperser::runPair(disp1_, disp2_, tap1, tap2, l.sections, l.chirpA);
    dly2_.write(tap1 + x);
    dly1_.write(tap2 + x);
    return (tap1 + tap2) * l.wetGain;
  }

  // Read advance that makes the half's delay plus its diffusers halfMs long,
  // with room left for the wobble to shorten it further.
  static float advance(float halfMs, int diffusers, float depth) {
    using namespace spring_detail;
    const float len = clampf(halfMs * kMsToSamples - static_cast<float>(diffusers),
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
  spring_detail::Disperser disp1_;
  spring_detail::Disperser disp2_;
  Line dly1_;
  Line dly2_;
  std::array<float, 257> sine_{};
  float lp1_ = 0.0f, lp2_ = 0.0f, hp1_ = 0.0f, hp2_ = 0.0f;
  uint32_t lfoAcc_ = 0u;
  int sections_ = 0;
  float decay_ = 0.0f, dwell_ = 0.0f;
  bool valid_ = false;
};

}  // namespace cv
