#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/smooth.h"
#include "engine/spring.h"

// Spring reverb option C (PARKER): the Välimäki / Parker / Abel parametric spring
// built from the research handover (docs/reference/spring-reverb-research.md §2, §9.3).
// Each spring is a fractional-stretch allpass chain in a Gajarsky multitap loop
// (C_lf) plus a plain-allpass splash loop (C_hf). Up to three detuned springs run
// interleaved behind a drive/recovery wrapper. Output is wet only.
// TENSION sets the loop gain; DWELL drives the soft clip. wetDb trims the output.

namespace cv {

struct SpringCTuning {
  float tdMs = 56.0f;                          // T_D, spring 1
  float fcLfHz = 4275.0f;                      // F_c,lf, sets K (fractional)
  int mLow = 100;                              // 1..100
  float aLf = 0.70f;
  float gLo = 0.35f, gHi = 0.82f;              // |g_lf| at TENSION 0 / 1, before gComp
  float gComp = 1.0f;                          // DAFx-11 uses 1.2 for loss this loop lacks
  float hfRatio = 1.3f;                        // g_hf / g_lf
  int mHigh = 189;                             // 0..200, 0 disables C_hf
  float aHf = -0.34f;
  float hfMixDb = -22.0f;                      // g_high re g_low
  float cross = 0.1f;                          // c1, C_hf into the C_lf summer
  float eqPeakHz = 183.0f, eqBwHz = 0.0f;      // chirp EQ (DAFx-11), bw 0 = off
  float lowHz = 4750.0f;                       // H_low cutoff; the table is fixed
  float echoGain = 0.2f, rippleGain = 0.2f;    // multitap e, r
  float modDepth = 8.0f, modPole = 0.93f;      // Gajarsky read modulation
  int springs = 3;                             // 1..3
  std::array<float, 3> tdFactor = {1.0f, 1.15f, 0.88f};
  std::array<float, 3> fcFactor = {1.0f, 0.98f, 1.02f};
  float hpHz = 150.0f, lpHz = 9000.0f;         // drive/recovery wrapper
  float dwellDrive = 32.0f, dwellComp = 0.80f;
  float presenceHz = 3000.0f, presenceDb = 5.0f, presenceQ = 1.0f;
  float tankTrim = 0.75f;                      // into the tank, after the clip
  float wetDb = 6.8f;                         // output trim; level rule at MIX 0.5
};

struct SpringCParams {
  float tension = 0.5f;  // panel knob 0..1; vocal default
  float dwell = 0.2f;     // panel knob 0..1; light drive
  SpringCTuning tuning;
};

namespace spring_c_detail {

using spring_detail::Biquad;
using spring_detail::clampf;
using spring_detail::dbToLin;
using spring_detail::onePole;
using spring_detail::softClip;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kFs = static_cast<float>(kSampleRate);
constexpr int kSprings = 3;
constexpr int kMaxSections = 100;
constexpr int kRing = 8;                         // K1 <= 8
constexpr int kMaxHf = 200;
constexpr int kMaxLoop = 3200;                   // L, samples
constexpr float kMaxMod = 16.0f;                 // read offset bound, samples
constexpr int kDlyBuf = kMaxLoop + 24;           // L + mod + interpolation slack
constexpr int kMaxHfLoop = kMaxLoop * 10 / 23 + 1;
constexpr int kHfBuf = kMaxHfLoop + 1;
constexpr int kEqBuf = 32;                       // > 2 * K_eq, power of two
constexpr int kLowRows = 5;
constexpr float kMaxGain = 0.97f;               // |g_lf| and |g_hf| rail
constexpr float kDcHz = 40.0f;

// Feedback sign (handover §2.3). The summer is s = x + kLoopSign·g·d with g negative.
// kLoopSign = +1 is net negative feedback, so the first echo correlates negatively
// with the direct path (DAFx-11: sgn g = sgn of the ACF maximum). With -1 the echo
// correlates positively even though its largest sample happens to be negative.
constexpr float kLoopSign = 1.0f;

// H_low (§2.3). scipy.signal.ellip(10, 1, 60, 4750, fs=48000, output='sos'), rows {b0, b1, b2, a1, a2}.
constexpr std::array<std::array<float, 5>, kLowRows> kLow = {{
    {2.347440819e-03f, 9.108687622e-04f, 2.347440819e-03f, -1.686439625e+00f, 7.363054338e-01f},
    {1.000000000e+00f, -1.246292218e+00f, 1.000000000e+00f, -1.656426097e+00f, 8.467574505e-01f},
    {1.000000000e+00f, -1.515232208e+00f, 1.000000000e+00f, -1.632407455e+00f, 9.363177933e-01f},
    {1.000000000e+00f, -1.582644451e+00f, 1.000000000e+00f, -1.622367722e+00f, 9.777837304e-01f},
    {1.000000000e+00f, -1.601133904e+00f, 1.000000000e+00f, -1.621570351e+00f, 9.947415935e-01f},
}};

inline int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline int roundi(float x) { return static_cast<int>(floorf(x + 0.5f)); }

// Per-spring loop geometry (§2.2, spec "Behaviour per spring" 1).
struct Geometry {
  int k1 = 5;
  float a2 = 0.0f;
  int l0 = 0, lRipple = 0, lEcho = 0, lHf = 1;
  int kEq = 5;
  bool eq = false;
  float eqGain = 0.0f, eqA1 = 0.0f, eqA2 = 0.0f;
};

inline Geometry geometry(const SpringCTuning& t, int spring, int mLow) {
  Geometry g;
  const float fc = clampf(t.fcLfHz * t.fcFactor[static_cast<size_t>(spring)], 100.0f, 20000.0f);
  const float k = clampf(kFs / (2.0f * fc), 1.5f, kRing + 0.49f);
  g.k1 = clampi(roundi(k) - 1, 1, kRing);
  const float d = k - static_cast<float>(g.k1);
  g.a2 = (1.0f - d) / (1.0f + d);

  const float a = clampf(t.aLf, -0.99f, 0.99f);
  const float tauDc = k * static_cast<float>(mLow) * (1.0f - a) / (1.0f + a);
  const float tdSamples = t.tdMs * t.tdFactor[static_cast<size_t>(spring)] * kFs / 1000.0f;
  g.lRipple = roundi(k);
  int l = roundi(tdSamples - tauDc);
  g.lEcho = roundi(static_cast<float>(l) / 5.0f);
  const int lMin = g.lEcho + g.lRipple + static_cast<int>(kMaxMod) + 2;
  l = clampi(l, lMin, kMaxLoop);
  g.lEcho = clampi(roundi(static_cast<float>(l) / 5.0f), 0, l - g.lRipple - static_cast<int>(kMaxMod) - 2);
  g.l0 = l - g.lEcho - g.lRipple;
  g.lHf = clampi(roundi(static_cast<float>(l) / 2.3f), 1, kMaxHfLoop);

  // Chirp EQ: resonator stretched by K_eq = floor(K) (§2.3 H_eq).
  g.kEq = clampi(static_cast<int>(floorf(k)), 1, kRing);
  g.eq = t.eqBwHz > 0.0f && t.eqPeakHz > 0.0f;
  if (g.eq) {
    const float ke = static_cast<float>(g.kEq);
    const float r = clampf(1.0f - kPi * t.eqBwHz * ke / kFs, 0.0f, 0.9999f);
    const float pcos = ((1.0f + r * r) / (2.0f * r)) * cosf(2.0f * kPi * t.eqPeakHz * ke / kFs);
    g.eqGain = 0.5f * (1.0f - r * r);
    g.eqA1 = -2.0f * r * pcos;
    g.eqA2 = r * r;
  }
  return g;
}

// One spring's state: C_lf chain, loop delay, C_hf chain and delay, output path.
struct Tank {
  std::array<float, kMaxSections * kRing> ring{};  // K1-deep ring per section
  std::array<float, kMaxSections> fd{};             // A_fd(a2) state per section
  std::array<float, kMaxHf> hfAp{};
  std::array<float, kDlyBuf> dly{};
  std::array<float, kHfBuf> hf{};
  std::array<float, kEqBuf> eqX{}, eqY{};
  std::array<Biquad, kLowRows> low;
  Geometry geo;
  int ringPos = 0, dlyPos = 0, hfPos = 0, eqPos = 0;
  float dcX = 0.0f, dcY = 0.0f;
  float noise = 0.0f;
  uint32_t seed = 1u, rng = 1u;

  void clear() {
    ring.fill(0.0f);
    fd.fill(0.0f);
    hfAp.fill(0.0f);
    dly.fill(0.0f);
    hf.fill(0.0f);
    eqX.fill(0.0f);
    eqY.fill(0.0f);
    for (Biquad& b : low) b.clear();
    ringPos = dlyPos = hfPos = eqPos = 0;
    dcX = dcY = noise = 0.0f;
    rng = seed;
  }

  // Linear read, delay counted back from the slot written this sample.
  float read(float delay) const {
    float p = static_cast<float>(dlyPos) - delay;
    if (p < 0.0f) p += static_cast<float>(kDlyBuf);
    int i0 = static_cast<int>(p);
    const float fr = p - static_cast<float>(i0);
    if (i0 >= kDlyBuf) i0 -= kDlyBuf;
    const int i1 = i0 + 1 >= kDlyBuf ? 0 : i0 + 1;
    const float v0 = dly[static_cast<size_t>(i0)];
    return v0 + fr * (dly[static_cast<size_t>(i1)] - v0);
  }

  // One-pole-filtered white noise in [-1, 1] (§2.3 modulation).
  float stepNoise(float pole) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    const float w = static_cast<float>(static_cast<int32_t>(rng)) * (1.0f / 2147483648.0f);
    noise = pole * noise + (1.0f - pole) * w;
    return noise;
  }

  // Stretched resonator y = G(x - x[n-2K]) - a1 y[n-K] - a2 y[n-2K].
  float runEq(float x) {
    const int k = geo.kEq;
    const float y = geo.eqGain * (x - eqX[static_cast<size_t>((eqPos - 2 * k) & (kEqBuf - 1))]) -
                    geo.eqA1 * eqY[static_cast<size_t>((eqPos - k) & (kEqBuf - 1))] -
                    geo.eqA2 * eqY[static_cast<size_t>((eqPos - 2 * k) & (kEqBuf - 1))];
    eqX[static_cast<size_t>(eqPos)] = x;
    eqY[static_cast<size_t>(eqPos)] = y;
    eqPos = (eqPos + 1) & (kEqBuf - 1);
    return y;
  }
};

// Shared per-sample loop settings.
struct Frame {
  float tap[4];  // multitap gains with g_lf and the loop sign folded in
  float fbHf;    // C_hf feedback, loop sign folded in
  float hfMix, cross;
  float a1, a2Hf;
  float dcGain, dcCoef;
  float modDepth, modPole;
  bool mod;
  int mLow, mHigh;
};

}  // namespace spring_c_detail

class SpringC {
 public:
  SpringC() {
    using namespace spring_c_detail;
    constexpr std::array<uint32_t, kSprings> kSeeds = {0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u};
    const SpringCTuning t;
    for (int j = 0; j < kSprings; ++j) {
      Tank& k = tanks_[static_cast<size_t>(j)];
      k.seed = kSeeds[static_cast<size_t>(j)];
      for (int r = 0; r < kLowRows; ++r) k.low[static_cast<size_t>(r)].set(kLow[static_cast<size_t>(r)]);
      k.geo = geometry(t, j, t.mLow);
    }
    reset();
  }

  void reset() {
    for (spring_c_detail::Tank& t : tanks_) t.clear();
    presence_.clear();
    live_ = {false, false, false};
    hp_ = lp_ = 0.0f;
    tension_ = dwell_ = 0.0f;
    valid_ = false;
  }

  void process(const float* in, float* wet, int n, const SpringCParams& p) {
    using namespace spring_c_detail;
    const SpringCTuning& t = p.tuning;
    const float tensionTarget = smooth::clamp01(p.tension);
    const float dwellTarget = smooth::clamp01(p.dwell);
    if (!valid_) {
      tension_ = tensionTarget;
      dwell_ = dwellTarget;
      valid_ = true;
    }

    const int springs = clampi(t.springs, 1, kSprings);
    const std::array<bool, kSprings> use = {solo_ < 0 || solo_ == 0,
                                            solo_ == 1 || (solo_ < 0 && springs >= 2),
                                            solo_ == 2 || (solo_ < 0 && springs >= 3)};
    std::array<int, kSprings> act{};
    int nAct = 0;
    Frame f{};
    f.mLow = clampi(t.mLow, 1, kMaxSections);
    f.mHigh = clampi(t.mHigh, 0, kMaxHf);
    for (int j = 0; j < kSprings; ++j) {
      if (!use[static_cast<size_t>(j)]) continue;
      Tank& k = tanks_[static_cast<size_t>(j)];
      const Geometry g = geometry(t, j, f.mLow);
      // A spring entering the mix, or one whose ring depth or EQ stretch moves, starts from silence.
      if (!live_[static_cast<size_t>(j)] || g.k1 != k.geo.k1 || g.kEq != k.geo.kEq) k.clear();
      k.geo = g;
      act[static_cast<size_t>(nAct++)] = j;
    }
    live_ = use;
    const float sumScale = dbToLin(t.wetDb) / static_cast<float>(nAct);

    // Block constants.
    const float a = smooth::coef(smooth::kSmoothSec);
    const float hpCoef = onePole(clampf(t.hpHz, 10.0f, 2000.0f));
    const float lpCoef = onePole(clampf(t.lpHz, 500.0f, 20000.0f));
    const float logDrive = logf(t.dwellDrive < 1.0f ? 1.0f : t.dwellDrive);
    const float e = clampf(t.echoGain, 0.0f, 1.0f);
    const float r = clampf(t.rippleGain, 0.0f, 1.0f);
    const float tapNorm = 1.0f / ((1.0f + e) * (1.0f + r));
    const float trim = t.tankTrim;
    presence_.setPeak(clampf(t.presenceHz, 100.0f, 12000.0f), t.presenceDb,
                      clampf(t.presenceQ, 0.1f, 10.0f));
    f.hfMix = dbToLin(t.hfMixDb);
    f.cross = t.cross;
    f.a1 = clampf(t.aLf, -0.99f, 0.99f);
    f.a2Hf = clampf(t.aHf, -0.99f, 0.99f);
    // H_dc (§2.3): adc = tan(π/4 − π·40/fs), y = ½(1 + adc)(x − x1) + adc·y1.
    f.dcCoef = tanf(kPi / 4.0f - kPi * kDcHz / kFs);
    f.dcGain = 0.5f * (1.0f + f.dcCoef);
    f.mod = modOn_ && t.modDepth > 0.0f;
    f.modDepth = clampf(t.modDepth, 0.0f, kMaxMod);
    f.modPole = clampf(t.modPole, 0.0f, 0.9999f);
    chainSections_ = f.mLow;
    chainA1_ = f.a1;

    float drive = 1.0f, comp = 1.0f;
    float lastTension = -1.0f, lastDwell = -1.0f;
    for (int i = 0; i < n; ++i) {
      tension_ = smooth::step(tension_, tensionTarget, a);
      dwell_ = smooth::step(dwell_, dwellTarget, a);
      if (tension_ != lastTension) {
        lastTension = tension_;
        // g_lf = −(gLo + x(gHi − gLo))·gComp, g_hf = hfRatio·g_lf, both railed at 0.97.
        // Taps divide by (1 + e)(1 + r), so the loop gain never exceeds |g_lf|.
        const float gRaw = -(t.gLo + tension_ * (t.gHi - t.gLo)) * t.gComp;
        const float g = clampf(gRaw, -kMaxGain, kMaxGain);
        const float fb = kLoopSign * g * tapNorm;
        f.tap[0] = fb * e * r;
        f.tap[1] = fb * e;
        f.tap[2] = fb * r;
        f.tap[3] = fb;
        f.fbHf = kLoopSign * clampf(t.hfRatio * gRaw, -kMaxGain, kMaxGain);
      }
      if (dwell_ != lastDwell) {
        lastDwell = dwell_;
        drive = expf(logDrive * dwell_);
        comp = expf(-t.dwellComp * logDrive * dwell_);
      }

      // Drive/recovery front end (§7): hp1 → drive → soft clip → comp → lp1.
      const float xin = in[i];
      hp_ += (xin - hp_) * hpCoef;
      const float clipped = softClip((xin - hp_) * drive) * comp;
      lp_ += (clipped - lp_) * lpCoef;
      const float x = lp_ * trim;

      wet[i] = presence_.run(tick(x, f, act, nAct) * sumScale);
    }
  }

  // Test hooks.
  void setModEnabled(bool on) { modOn_ = on; }
  void setSolo(int spring) { solo_ = spring < 0 || spring > 2 ? -1 : spring; }

  // Impulse response of spring's C_lf section chain alone, geometry from the last
  // process() call (defaults before one). Clears that spring.
  void chainImpulse(int spring, float* buf, int n) {
    using namespace spring_c_detail;
    spring_c_detail::Tank& k = tanks_[static_cast<size_t>(clampi(spring, 0, kSprings - 1))];
    k.clear();
    const float a1 = chainA1_;
    for (int i = 0; i < n; ++i) {
      float v = i == 0 ? 1.0f : 0.0f;
      for (int m = 0; m < chainSections_; ++m) v = section(k, m, v, a1);
      if (++k.ringPos >= k.geo.k1) k.ringPos = 0;
      buf[i] = v;
    }
    k.clear();
  }

 private:
  // C_lf section (§2.2): first-order allpass a1 around z^-K1 · A_fd(a2).
  static float section(spring_c_detail::Tank& k, int m, float v, float a1) {
    float* ring = &k.ring[static_cast<size_t>(m * spring_c_detail::kRing + k.ringPos)];
    float& fd = k.fd[static_cast<size_t>(m)];
    const float old = *ring;
    const float q = k.geo.a2 * old + fd;
    fd = old - k.geo.a2 * q;
    const float y = a1 * v + q;
    *ring = v - a1 * y;
    return y;
  }

  // One sample through every active spring; returns the unscaled spring sum.
  float tick(float x, const spring_c_detail::Frame& f, const std::array<int, 3>& act, int nAct) {
    using namespace spring_c_detail;
    std::array<float, kSprings> hv{}, lv{};

    // C_hf (§2.3): plain allpasses in a loop of L/2.3.
    for (int j = 0; j < nAct; ++j) {
      Tank& k = tanks_[static_cast<size_t>(act[static_cast<size_t>(j)])];
      int rd = k.hfPos - k.geo.lHf;
      if (rd < 0) rd += kHfBuf;
      hv[static_cast<size_t>(j)] = x + f.fbHf * k.hf[static_cast<size_t>(rd)];
    }
    for (int m = 0; m < f.mHigh; ++m) {
      for (int j = 0; j < nAct; ++j) {
        float& st = tanks_[static_cast<size_t>(act[static_cast<size_t>(j)])].hfAp[static_cast<size_t>(m)];
        float& v = hv[static_cast<size_t>(j)];
        const float y = f.a2Hf * v + st;
        st = v - f.a2Hf * y;
        v = y;
      }
    }

    // C_lf summer (§2.3): input, Gajarsky multitap, C_hf cross-feed, then H_dc.
    for (int j = 0; j < nAct; ++j) {
      Tank& k = tanks_[static_cast<size_t>(act[static_cast<size_t>(j)])];
      k.hf[static_cast<size_t>(k.hfPos)] = hv[static_cast<size_t>(j)];
      if (++k.hfPos >= kHfBuf) k.hfPos = 0;

      const float mod = f.mod ? f.modDepth * k.stepNoise(f.modPole) : 0.0f;
      const float d0 = static_cast<float>(k.geo.l0) + mod;
      const float dR = static_cast<float>(k.geo.lRipple);
      const float dE = static_cast<float>(k.geo.lEcho);
      const float d = f.tap[0] * k.read(d0) + f.tap[1] * k.read(d0 + dR) +
                      f.tap[2] * k.read(d0 + dE) + f.tap[3] * k.read(d0 + dE + dR);
      const float s = x + d + f.cross * hv[static_cast<size_t>(j)];
      k.dcY = f.dcGain * (s - k.dcX) + f.dcCoef * k.dcY;
      k.dcX = s;
      lv[static_cast<size_t>(j)] = k.dcY;
    }

    // Stretched chain, springs interleaved per section.
    for (int m = 0; m < f.mLow; ++m) {
      for (int j = 0; j < nAct; ++j) {
        Tank& k = tanks_[static_cast<size_t>(act[static_cast<size_t>(j)])];
        lv[static_cast<size_t>(j)] = section(k, m, lv[static_cast<size_t>(j)], f.a1);
      }
    }

    // Loop write, output path H_eq → H_low, C_hf mixed in.
    float sum = 0.0f;
    for (int j = 0; j < nAct; ++j) {
      Tank& k = tanks_[static_cast<size_t>(act[static_cast<size_t>(j)])];
      const float u = lv[static_cast<size_t>(j)];
      if (++k.ringPos >= k.geo.k1) k.ringPos = 0;
      k.dly[static_cast<size_t>(k.dlyPos)] = u;
      if (++k.dlyPos >= kDlyBuf) k.dlyPos = 0;
      float o = k.geo.eq ? k.runEq(u) : u;
      for (Biquad& b : k.low) o = b.run(o);
      sum += o + f.hfMix * hv[static_cast<size_t>(j)];
    }
    return sum;
  }

  std::array<spring_c_detail::Tank, spring_c_detail::kSprings> tanks_;
  spring_c_detail::Biquad presence_;
  std::array<bool, 3> live_{};
  float hp_ = 0.0f, lp_ = 0.0f;
  float tension_ = 0.0f, dwell_ = 0.0f;
  int chainSections_ = spring_c_detail::kMaxSections;
  float chainA1_ = 0.63f;
  bool valid_ = false;
  bool modOn_ = true;
  int solo_ = -1;
};

}  // namespace cv
