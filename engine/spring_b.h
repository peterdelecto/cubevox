#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/chasm.h"
#include "engine/common.h"
#include "engine/smooth.h"

// SPRING B reverb: the CHASM loop voiced as a spring, cheap enough for the H7.
//
// Loop: dly1 -> filters -> diffusers 1a, 1b -> disperser 1 -> dly2 -> filters
// -> diffusers 2a, 2b -> disperser 2 -> dly1. Each half is one echo, so the
// taps after the dispersers repeat at halfMs1 and halfMs2 apart, a spring's
// flutter. The dispersers are stretched allpasses (a + z^-K) / (1 + a z^-K)
// with a > 0, so lows arrive first and each echo sweeps up. They sit inside
// the loop, so every echo picks up more sweep than the last: the drip.
// Light diffusers keep the echoes distinct. In-loop treble loss and bass cut
// darken and thin each pass. DECAY sets the feedback; the input scales by
// sqrt(1 - g^2) so the tail's level holds as DECAY moves. DWELL drives the
// CHASM soft clip. Output is wet only, at wetDb.

namespace cv {

struct SpringBTuning {
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
  float dwellDrive = 8.0f, dwellComp = 0.8f;  // input drive at DWELL 1; level comp exponent
  float wetDb = 7.9f;                      // trim; level rule at MIX 1 (wet only)
};

struct SpringBParams {
  float decay = 0.50f;  // set by INTENSITY
  float dwell = 0.20f;  // knob 2; input drive
  SpringBTuning tuning;
};

namespace spring_b_detail {

constexpr float kTwoPi = 6.28318530717958647692f;
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

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

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

  float run(float in, int sections, float a) {
    for (int u = 0; u < sections; ++u) {
      float& z = buf[static_cast<size_t>(u * kStretch + pos)];
      const float out = z + a * in;
      z = in - a * out;
      in = out;
    }
    if (++pos >= kStretch) pos = 0;
    return in;
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

}  // namespace spring_b_detail

class SpringB {
 public:
  SpringB() {
    for (int i = 0; i < 257; ++i)
      sine_[static_cast<size_t>(i)] =
          sinf(static_cast<float>(i) * (spring_b_detail::kTwoPi / 256.0f));
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
  void process(const float* in, float* wet, int n, const SpringBParams& p) {
    using namespace spring_b_detail;
    const SpringBTuning& t = p.tuning;
    const float decayTarget = smooth::clamp01(p.decay);
    const float dwellTarget = smooth::clamp01(p.dwell);
    if (!valid_) {
      decay_ = decayTarget;
      dwell_ = dwellTarget;
      valid_ = true;
    }

    // Block constants.
    const float a = smooth::coef(smooth::kSmoothSec);
    const float timeHi = clampf(t.timeHi, 0.0f, kTimeMax);
    const float timeLo = clampf(t.timeLo, 0.0f, timeHi);
    const float k = clampf(t.diffuse, 0.0f, 0.9f);
    const float chirpA = clampf(t.chirpA, 0.0f, 0.95f);
    const int sections = t.chirpSections < 0 ? 0 : (t.chirpSections > kMaxSections ? kMaxSections : t.chirpSections);
    if (sections > sections_) {
      disp1_.clearFrom(sections_);
      disp2_.clearFrom(sections_);
    }
    sections_ = sections;
    const float lpF = chasm_detail::onePole(t.trebleLossHz);
    const float hpF = chasm_detail::onePole(t.bassCutHz);
    const float depth = clampf(t.wobbleDepth, 0.0f, kMaxWobble);
    const uint32_t lfoStep = static_cast<uint32_t>(clampf(t.wobbleRateHz, 0.0f, 20.0f) * kPhasePerHz);
    const float adv1 = advance(t.halfMs1, kDiff1A + kDiff1B, depth);
    const float adv2 = advance(t.halfMs2, kDiff2A + kDiff2B, depth);
    const float logDrive = logf(t.dwellDrive < 1.0f ? 1.0f : t.dwellDrive);
    float lastDwell = -1.0f, drive = 1.0f, comp = 1.0f;
    const float wetGain = powf(10.0f, t.wetDb / 20.0f);

    for (int i = 0; i < n; ++i) {
      decay_ = smooth::step(decay_, decayTarget, a);
      dwell_ = smooth::step(dwell_, dwellTarget, a);
      if (dwell_ != lastDwell) {
        lastDwell = dwell_;
        drive = expf(logDrive * dwell_);
        comp = expf(-t.dwellComp * logDrive * dwell_);
      }
      const float g = timeLo + decay_ * (timeHi - timeLo);
      const float x = softClipHeadroom(in[i] * drive, kChasmClipHeadroom) *
                      comp * t.inputTrim * sqrtf(1.0f - g * g);
      lfoAcc_ += lfoStep;

      const float out1 = filter(dly1_.read(adv1 + lfo(0u) * depth) * g, lp1_, hp1_, lpF, hpF);
      const float tap1 = disp1_.run(d1b_.run(d1a_.run(out1, k), k), sections, chirpA);

      const float out2 = filter(dly2_.read(adv2 + lfo(64u) * depth) * g, lp2_, hp2_, lpF, hpF);
      dly2_.write(tap1 + x);
      const float tap2 = disp2_.run(d2b_.run(d2a_.run(out2, k), k), sections, chirpA);
      dly1_.write(tap2 + x);

      wet[i] = (tap1 + tap2) * wetGain;
    }
  }

 private:
  using Line = chasm_detail::Delay<spring_b_detail::kDlyLen>;

  // Read advance that makes the half's delay plus its diffusers halfMs long,
  // with room left for the wobble to shorten it further.
  static float advance(float halfMs, int diffusers, float depth) {
    using namespace spring_b_detail;
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

  spring_b_detail::Diffuser<spring_b_detail::kDiff1A> d1a_;
  spring_b_detail::Diffuser<spring_b_detail::kDiff1B> d1b_;
  spring_b_detail::Diffuser<spring_b_detail::kDiff2A> d2a_;
  spring_b_detail::Diffuser<spring_b_detail::kDiff2B> d2b_;
  spring_b_detail::Disperser disp1_;
  spring_b_detail::Disperser disp2_;
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
