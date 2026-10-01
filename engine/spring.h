#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/smooth.h"

// Spring reverb: Välimäki / Parker / Abel parametric spring model behind a
// 6G15-style driver. Each spring runs a low-band chirp loop (stretched allpass
// cascade inside a multitap feedback delay, elliptic band limit at F_C) and a
// high-band splash loop (high-pass at F_C, diffusers, own feedback delay).
// TENSION sets the loop feedback; DWELL drives the soft clip and raises the splash.
// Dry passes at unity; the wet sum is added at wetDb.

namespace cv {

struct SpringTuning {
  float hpHz = 300.0f;
  float tensionLo = 0.60f, tensionHi = 0.97f;   // |g| at TENSION 0 / 1
  float dwellDrive = 32.0f, dwellComp = 0.80f;
  float hfMixDbLo = -18.0f, hfMixDbHi = -8.0f;  // C_hf mix at DWELL 0 / 1
  float rippleGain = 0.20f;                     // pre-echo taps, paper 0.1
  float splashDiffuse = 0.50f;                  // 0 off .. 0.9
  int hfSections = 0;                           // 0..200
  int springs = 2;                              // 2 or 3
  float modDepth = 8.0f, modRateHz = 3.0f;
  float boingDb = 0.0f;                         // 95 Hz resonator, 0 = off
  float wetDb = -6.0f;
  float tankTrim = 0.375f;                      // DSV3: inTrim 0.25 x tankTrim 1.5
};

struct SpringParams {
  bool on = true;
  float tension = 0.5f;  // panel knob 0..1
  float dwell = 0.5f;    // panel knob 0..1
  SpringTuning tuning;
};

namespace spring_detail {

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr int kSections = 130;          // C_lf allpass cascade length M
constexpr float kA = 0.75f;             // positive, so lows arrive first
constexpr int kGuard = 32;              // delay slack for the wander
constexpr float kWanderMax = kGuard - 4;
constexpr int kRipple = kSampleRate / 1000;  // pre-echo ripple offset, 1 ms
constexpr int kHfMaxSections = 200;
constexpr float kHfA = 0.6f;
constexpr float kHfFbRatio = 0.77f / 0.80f;  // C_hf feedback : C_lf feedback
constexpr float kHfCross = 0.1f;             // C_hf into the C_lf summer
constexpr float kDcHz = 40.0f;
constexpr float kMaxFeedback = 0.995f;
constexpr int kDiffuseA = static_cast<int>(3.1f * kSampleRate / 1000.0f + 0.5f);
constexpr int kDiffuseB = static_cast<int>(7.3f * kSampleRate / 1000.0f + 0.5f);
constexpr float kBoingHz = 95.0f;
constexpr float kBoingQ = 95.0f / 130.0f;  // 130 Hz bandwidth
constexpr float kClipMax = 3.0f;

using Sos = std::array<std::array<float, 5>, 3>;  // rows {b0, b1, b2, a1, a2}

// scipy.signal.ellip(6, 0.5, 60, fs / 2K, fs=48000, output='sos')
constexpr Sos kEllipK5 = {{
    {3.170187928e-03f, 2.448448052e-03f, 3.170187928e-03f, -1.574285048e+00f, 6.496029638e-01f},
    {1.000000000e+00f, -9.573141810e-01f, 1.000000000e+00f, -1.551997326e+00f, 7.965495053e-01f},
    {1.000000000e+00f, -1.285106260e+00f, 1.000000000e+00f, -1.562862547e+00f, 9.396148292e-01f},
}};
constexpr Sos kEllipK6 = {{
    {2.383943886e-03f, 1.007344239e-03f, 2.383943886e-03f, -1.646843207e+00f, 7.000183821e-01f},
    {1.000000000e+00f, -1.226384803e+00f, 1.000000000e+00f, -1.652930569e+00f, 8.257039330e-01f},
    {1.000000000e+00f, -1.484337949e+00f, 1.000000000e+00f, -1.682745577e+00f, 9.483735958e-01f},
}};
constexpr Sos kEllipK7 = {{
    {1.954320361e-03f, 2.113410835e-04f, 1.954320361e-03f, -1.698075320e+00f, 7.376835735e-01f},
    {1.000000000e+00f, -1.407211516e+00f, 1.000000000e+00f, -1.719214914e+00f, 8.477613502e-01f},
    {1.000000000e+00f, -1.612118776e+00f, 1.000000000e+00f, -1.757949845e+00f, 9.550323535e-01f},
}};

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

inline float dbToLin(float db) { return powf(10.0f, db / 20.0f); }

// One-pole coefficient for a corner at hz.
inline float onePole(float hz) {
  const float a = 1.0f - expf(-kTwoPi * hz / kSampleRate);
  return a > 0.99f ? 0.99f : a;
}

// Padé tanh, clamped where it meets the rail.
inline float softClip(float x) {
  x = clampf(x, -kClipMax, kClipMax);
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// Transposed direct form II.
struct Biquad {
  float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
  float z1 = 0.0f, z2 = 0.0f;

  void set(const std::array<float, 5>& r) {
    b0 = r[0];
    b1 = r[1];
    b2 = r[2];
    a1 = r[3];
    a2 = r[4];
  }

  // RBJ high-pass.
  void setHighPass(float hz, float q) {
    const float w0 = kTwoPi * hz / kSampleRate;
    const float cw = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * q);
    const float a0 = 1.0f + alpha;
    b0 = 0.5f * (1.0f + cw) / a0;
    b1 = -(1.0f + cw) / a0;
    b2 = b0;
    a1 = -2.0f * cw / a0;
    a2 = (1.0f - alpha) / a0;
  }

  // RBJ peaking EQ.
  void setPeak(float hz, float db, float q) {
    const float w0 = kTwoPi * hz / kSampleRate;
    const float amp = powf(10.0f, db / 40.0f);
    const float cw = cosf(w0);
    const float alpha = sinf(w0) / (2.0f * q);
    const float a0 = 1.0f + alpha / amp;
    b0 = (1.0f + alpha * amp) / a0;
    b1 = -2.0f * cw / a0;
    b2 = (1.0f - alpha * amp) / a0;
    a1 = b1;
    a2 = (1.0f - alpha / amp) / a0;
  }

  float run(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  void clear() { z1 = z2 = 0.0f; }
};

// Schroeder allpass (-g + z^-N) / (1 - g z^-N).
template <int N>
struct Diffuser {
  std::array<float, N> buf{};
  int pos = 0;

  float run(float x, float g) {
    const float w = buf[pos];
    const float y = w - g * x;
    buf[pos] = x + g * y;
    if (++pos >= N) pos = 0;
    return y;
  }

  void clear() {
    buf.fill(0.0f);
    pos = 0;
  }
};

// Per-sample loop settings shared by every spring.
struct Frame {
  float tap0, tap1, tap3;  // multitap gains with the C_lf feedback folded in
  float gHf;               // C_hf feedback
  float hfMix;
  float diffuse;
  int hfSections;
  bool wander;
  float wanderCoef, wanderScale;
  float dcCoef;
};

inline const Sos& ellipFor(int k) { return k == 5 ? kEllipK5 : (k == 6 ? kEllipK6 : kEllipK7); }

// One spring. K stretches the allpasses (F_C = fs / 2K); L is the delay lap.
template <int K, int L>
class Tank {
 public:
  static_assert(K == 5 || K == 6 || K == 7, "elliptic tables exist for K = 5, 6, 7");

  void init(uint32_t seed) {
    seed_ = seed | 1u;
    hfHp_.setHighPass(static_cast<float>(kSampleRate) / (2.0f * K), 0.70710678f);
    const Sos& e = ellipFor(K);
    for (int s = 0; s < 3; ++s) lp_[s].set(e[s]);
    clear();
  }

  void clear() {
    ap_.fill(0.0f);
    dly_.fill(0.0f);
    hf_.fill(0.0f);
    hfAp_.fill(0.0f);
    diffA_.clear();
    diffB_.clear();
    hfHp_.clear();
    for (Biquad& b : lp_) b.clear();
    apPos_ = 0;
    dlyPos_ = 0;
    hfPos_ = 0;
    dc_ = 0.0f;
    fb_ = 0.0f;
    hfFb_ = 0.0f;
    wander_ = 0.0f;
    rng_ = seed_;
  }

  float process(float x, const Frame& f) {
    // C_hf: high band, diffused, recirculating through its own delay.
    float h = hfHp_.run(x);
    if (f.diffuse > 0.0f) h = diffB_.run(diffA_.run(h, f.diffuse), f.diffuse);
    float hv = h + hfFb_;
    for (int s = 0; s < f.hfSections; ++s) {
      const float y = kHfA * hv + hfAp_[s];
      hfAp_[s] = hv - kHfA * y;
      hv = y;
    }
    hf_[hfPos_] = hv;
    int r = hfPos_ - kHfLen;
    if (r < 0) r += kHfBuf;
    hfFb_ = hf_[r] * f.gHf;
    if (++hfPos_ >= kHfBuf) hfPos_ = 0;

    // C_lf: summer, DC block, dispersive cascade. The tap sits before the delay.
    float v = x + fb_ + kHfCross * hv;
    dc_ += (v - dc_) * f.dcCoef;
    v -= dc_;
    float* b = &ap_[static_cast<size_t>(apPos_)];
    for (int s = 0; s < kSections; ++s) {
      const float y = kA * v + *b;
      *b = v - kA * y;
      v = y;
      b += K;
    }
    if (++apPos_ >= K) apPos_ = 0;
    const float tap = v;

    dly_[static_cast<size_t>(dlyPos_)] = tap;
    float len = static_cast<float>(L);
    if (f.wander) {
      rng_ ^= rng_ << 13;
      rng_ ^= rng_ >> 17;
      rng_ ^= rng_ << 5;
      const float u = static_cast<float>(static_cast<int32_t>(rng_)) * (1.0f / 2147483648.0f);
      wander_ += (u - wander_) * f.wanderCoef;
      len += clampf(wander_ * f.wanderScale, -kWanderMax, kWanderMax);
    }
    fb_ = f.tap0 * read(len) + f.tap1 * (read(len - kPre) + read(len - kRipple)) +
          f.tap3 * read(len - kPre - kRipple);
    if (++dlyPos_ >= kDlyBuf) dlyPos_ = 0;

    float o = tap;
    for (Biquad& q : lp_) o = q.run(o);
    return o + f.hfMix * hv;
  }

 private:
  static constexpr int kDlyBuf = L + kGuard;
  static constexpr int kPre = L / 5;
  static constexpr int kHfLen = L * 10 / 23;
  static constexpr int kHfBuf = kHfLen + 8;

  // Linear read, delay measured back from the newest write.
  float read(float delay) const {
    float p = static_cast<float>(dlyPos_) - delay;
    if (p < 0.0f) p += static_cast<float>(kDlyBuf);
    int i0 = static_cast<int>(p);
    const float fr = p - static_cast<float>(i0);
    if (i0 >= kDlyBuf) i0 -= kDlyBuf;
    const int i1 = i0 + 1 >= kDlyBuf ? 0 : i0 + 1;
    return dly_[static_cast<size_t>(i0)] +
           fr * (dly_[static_cast<size_t>(i1)] - dly_[static_cast<size_t>(i0)]);
  }

  std::array<float, kSections * K> ap_{};
  std::array<float, kDlyBuf> dly_{};
  std::array<float, kHfBuf> hf_{};
  std::array<float, kHfMaxSections> hfAp_{};
  Diffuser<kDiffuseA> diffA_;
  Diffuser<kDiffuseB> diffB_;
  Biquad hfHp_;
  std::array<Biquad, 3> lp_;
  int apPos_ = 0, dlyPos_ = 0, hfPos_ = 0;
  float dc_ = 0.0f, fb_ = 0.0f, hfFb_ = 0.0f, wander_ = 0.0f;
  uint32_t seed_ = 1u, rng_ = 1u;
};

}  // namespace spring_detail

class Spring {
 public:
  Spring() {
    a_.init(0x9E3779B9u);
    b_.init(0x85EBCA6Bu);
    c_.init(0xC2B2AE35u);
    reset();
  }

  void reset() {
    a_.clear();
    b_.clear();
    c_.clear();
    boing_.clear();
    live_ = {true, true, false};
    hp_ = 0.0f;
    tension_ = dwell_ = active_ = 0.0f;
    valid_ = false;
  }

  void process(const float* in, float* out, int n, const SpringParams& p) {
    using namespace spring_detail;
    const SpringTuning& t = p.tuning;
    const float tensionTarget = smooth::clamp01(p.tension);
    const float dwellTarget = smooth::clamp01(p.dwell);
    const float activeTarget = p.on ? 1.0f : 0.0f;
    if (!valid_) {
      tension_ = tensionTarget;
      dwell_ = dwellTarget;
      active_ = activeTarget;
      valid_ = true;
    }

    // Block constants.
    const float a = smooth::coef(smooth::kSmoothSec);
    const float hpCoef = onePole(clampf(t.hpHz, 10.0f, 2000.0f));
    const float fbLo = clampf(t.tensionLo, 0.0f, kMaxFeedback);
    const float fbHi = clampf(t.tensionHi, 0.0f, kMaxFeedback);
    const float r = clampf(t.rippleGain, 0.0f, 1.0f);
    const float norm = 1.0f / ((1.0f + r) * (1.0f + r));
    const float logDrive = logf(t.dwellDrive < 1.0f ? 1.0f : t.dwellDrive);
    const float comp = t.dwellComp;
    const float wet = dbToLin(t.wetDb);
    const int springs = t.springs >= 3 ? 3 : 2;
    const bool boing = t.boingDb != 0.0f;
    if (boing) boing_.setPeak(kBoingHz, t.boingDb, kBoingQ);

    Frame f{};
    f.diffuse = clampf(t.splashDiffuse, 0.0f, 0.9f);
    f.hfSections = t.hfSections < 0 ? 0 : (t.hfSections > kHfMaxSections ? kHfMaxSections : t.hfSections);
    f.wander = modOn_ && t.modDepth > 0.0f;
    f.wanderCoef = onePole(clampf(t.modRateHz, 0.01f, 100.0f));
    f.wanderScale = t.modDepth * wanderNorm(f.wanderCoef);
    f.dcCoef = onePole(kDcHz);

    // A spring entering the mix starts from silence.
    const std::array<bool, 3> use = {solo_ < 0 || solo_ == 0, solo_ < 0 || solo_ == 1,
                                     solo_ == 2 || (solo_ < 0 && springs == 3)};
    if (use[0] && !live_[0]) a_.clear();
    if (use[1] && !live_[1]) b_.clear();
    if (use[2] && !live_[2]) c_.clear();
    live_ = use;
    const float sumScale = solo_ >= 0 ? 1.0f : 1.0f / static_cast<float>(springs);

    float drive = 1.0f, driveComp = 1.0f;
    float lastTension = -1.0f, lastDwell = -1.0f;
    for (int i = 0; i < n; ++i) {
      tension_ = smooth::step(tension_, tensionTarget, a);
      dwell_ = smooth::step(dwell_, dwellTarget, a);
      active_ = smooth::step(active_, activeTarget, a);

      if (tension_ != lastTension) {
        lastTension = tension_;
        const float g = -(fbLo + tension_ * (fbHi - fbLo));
        f.tap0 = g * norm;
        f.tap1 = g * norm * r;
        f.tap3 = g * norm * r * r;
        f.gHf = g * kHfFbRatio;
      }
      if (dwell_ != lastDwell) {
        lastDwell = dwell_;
        drive = expf(logDrive * dwell_);
        driveComp = expf(-comp * logDrive * dwell_) * t.tankTrim;
        f.hfMix = dbToLin(t.hfMixDbLo + dwell_ * (t.hfMixDbHi - t.hfMixDbLo));
      }

      hp_ += (in[i] - hp_) * hpCoef;
      const float x = softClip((in[i] - hp_) * drive) * driveComp;
      float sum = 0.0f;
      if (use[0]) sum += a_.process(x, f);
      if (use[1]) sum += b_.process(x, f);
      if (use[2]) sum += c_.process(x, f);
      sum *= sumScale;
      if (boing) sum = boing_.run(sum);
      out[i] = in[i] + active_ * wet * sum;
    }
  }

  // Test hooks.
  void setModEnabled(bool on) { modOn_ = on; }
  void setSolo(int spring) { solo_ = spring < 0 || spring > 2 ? -1 : spring; }

 private:
  // Holds the wander depth constant across rates; depth is calibrated at coefficient 0.07.
  static float wanderNorm(float c) {
    constexpr float kRef = 0.07f;
    return sqrtf((kRef / (2.0f - kRef)) * ((2.0f - c) / c));
  }

  spring_detail::Tank<5, 1827> a_;
  spring_detail::Tank<6, 2001> b_;
  spring_detail::Tank<7, 2174> c_;
  spring_detail::Biquad boing_;
  std::array<bool, 3> live_{};
  float hp_ = 0.0f;
  float tension_ = 0.0f, dwell_ = 0.0f, active_ = 0.0f;
  bool valid_ = false;
  bool modOn_ = true;
  int solo_ = -1;
};

}  // namespace cv
