#pragma once

#include <array>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"

// The Crowd card's trial voice (voice 5) is one more copy that tries to sound like a person of another
// sex, age or body size. These are its pieces. GrainLuck gives each grain a random length and loudness
// and drops pulses for vocal fry. FryGate decides when a phrase is ending. SpectralTilt leans the whole
// spectrum, and TrialChain adds breath under the copy. The four main copies share SpectralTilt and the
// shimmer of GrainLuck. Every piece is neutral at its zero, and the engine clamps each control to a
// human range.

namespace cv {
namespace crowd_detail {

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

// One-pole coefficient for a time constant in ms, from the series of 1 - exp(-x). It stays within float
// rounding for the time constants used here and needs no libm.
constexpr float tauCoef(float ms) {
  const float x = 1.0f / (ms * (kSampleRate / 1000.0f));
  return x * (1.0f - x * (0.5f - x * (1.0f / 6.0f)));
}

// Follows the level of |x|, rising by `up` and falling by `down` per sample.
struct EnvFollower {
  float y = 0.0f;

  CV_INLINE float run(float x, float up, float down) {
    const float a = x < 0.0f ? -x : x;
    y += (a > y ? up : down) * (a - y);
    return y;
  }

  void clear() { y = 0.0f; }
};

// The breath envelope follows the copy fast on the way up and slowly on the way down, so the noise
// swells with a syllable and dies with it.
constexpr float kBreathUp = tauCoef(10.0f);
constexpr float kBreathDown = tauCoef(60.0f);

// Jitter and shimmer are quoted as local values, the mean gap between neighbouring cycles. A uniform
// draw within +-kLocalPeak x s has neighbours that differ by s on average, so the setting reads as the
// number voice analysis reports.
constexpr float kLocalPeak = 1.5f;

// Per-grain luck. A voice asks it once per launch for the grain's loudness (0 drops the grain) and for
// the length of the cycle that follows. With every control at 0 it returns 1 and 1, and the voice lays
// down the grains it lays down without it. The trial voice uses all of it. A main copy sets only the
// shimmer.
struct GrainLuck {
  XorShift rng;
  float jitter = 0.0f;      // local jitter as a fraction of the cycle
  float shimmerDb = 0.0f;   // local shimmer in dB
  float dropChance = 0.0f;  // while frying, the chance that an alternate pulse is dropped
  float fryDb = 0.0f;       // while frying, the peak loudness swing of the kept pulses
  bool frying = false;
  bool odd = false;         // the next fry pulse is one of the alternates

  void seed(uint32_t s) {
    rng.seed(s);
    odd = false;
  }

  // The widest scale cycleScale() returns, so the voice's launch clock allows for it.
  float maxScale() const { return 1.0f + kLocalPeak * jitter; }

  float cycleScale() { return jitter > 0.0f ? 1.0f + kLocalPeak * jitter * rng.next() : 1.0f; }

  float grainGain() {
    float g = 1.0f;
    if (shimmerDb > 0.0f) g *= dbToGainFast(kLocalPeak * shimmerDb * rng.next());
    if (!frying) return g;
    odd = !odd;
    if (odd && 0.5f + 0.5f * rng.next() < dropChance) return 0.0f;
    return fryDb > 0.0f ? g * dbToGainFast(fryDb * rng.next()) : g;
  }
};

// Finds the end of a phrase. Vocal fry sets in when the pitch has dropped well below its running mean,
// the period longer than the mean by 1.15, while the level falls, and it lasts kFryMs. The copy
// plays its lead's words a lag late, so a window that opens at the detection still lands on the last
// words of the phrase. The gate rearms once the pitch comes back up.
class FryGate {
 public:
  static constexpr float kFryDevOct = 0.2016f;  // log2(1.15)
  static constexpr float kFallRatio = 0.85f;    // the fast level under this share of the slow one
  static constexpr int kFryMs = 150;

  void reset() {
    fast_.clear();
    slow_.clear();
    left_ = 0;
    armed_ = true;
  }

  // Per block. in is the lead and devOct the pitch against its running mean in octaves, negative when
  // the period is longer. Returns whether the window is open for this block.
  bool step(const float* in, int n, float devOct) {
    trackLevel(in, n);
    if (devOct >= -kFryDevOct) {
      armed_ = true;
    } else if (armed_ && left_ <= 0 && fast_.y < kFallRatio * slow_.y) {
      armed_ = false;
      left_ = kFryMs * kSampleRate / 1000;
    }
    const bool inside = left_ > 0;
    left_ = left_ > n ? left_ - n : 0;
    return inside;
  }

 private:
  static constexpr float kSlowCoef = tauCoef(150.0f);

  void trackLevel(const float* in, int n) {
    EnvFollower fast = fast_;
    EnvFollower slow = slow_;
    for (int i = 0; i < n; ++i) {
      fast.run(in[i], kBreathUp, kBreathDown);
      slow.run(in[i], kSlowCoef, kSlowCoef);
    }
    fast_ = fast;
    slow_ = slow;
  }

  EnvFollower fast_;
  EnvFollower slow_;
  int left_ = 0;
  bool armed_ = true;
};

// A low shelf at -db / 2 and a high shelf at +db / 2, both at 1 kHz, lean the whole spectrum so the high
// end sits db above the low end. A tilt under kOffDb counts as 0 and the stage is skipped, so a neutral
// tilt leaves the samples alone.
class SpectralTilt {
 public:
  static constexpr float kHz = 1000.0f;
  static constexpr float kOffDb = 0.005f;

  void clear() {
    lo_.clear();
    hi_.clear();
    db_ = 0.0f;
  }

  // Sets the shelves when the gain moved. Each setter runs libm, so nothing here belongs in a
  // per-sample loop. A stage that wakes from off starts with empty filter state.
  void set(float db) {
    if (db == db_) return;
    if (!on(db_)) {
      lo_.clear();
      hi_.clear();
    }
    lo_.setLowShelf(kHz, -0.5f * db);
    hi_.setHighShelf(kHz, 0.5f * db);
    db_ = db;
  }

  bool active() const { return on(db_); }

  // Leans n samples in place.
  void run(float* x, int n) {
    if (!active()) return;
    runOne(lo_, x, n);
    runOne(hi_, x, n);
  }

 private:
  static bool on(float db) { return db > kOffDb || db < -kOffDb; }

  static void runOne(Biquad& b, float* x, int n) {
    Biquad f = b;
    for (int i = 0; i < n; ++i) x[i] = f.run(x[i]);
    b = f;
  }

  Biquad lo_;
  Biquad hi_;
  float db_ = 0.0f;
};

// The trial voice's own colour. A spectral tilt leans the whole spectrum and breath noise rides under
// the copy. A tilt of 0 dB is skipped and breath 0 does not run, so a neutral chain is a copy.
class TrialChain {
 public:
  static constexpr float kBreathHz = 2000.0f;

  void clear() {
    tilt_.clear();
    breathHp_.clear();
    env_.clear();
    rng_.seed(0x6D2B79F5u);
    breathSet_ = false;
  }

  void set(float tiltDb) { tilt_.set(tiltDb); }

  // Shapes n samples in place, then mixes breath under them. breath 0 skips the noise entirely.
  void shape(float* x, int n, float breath) {
    tilt_.run(x, n);
    if (breath > 0.0f) addBreath(x, n, breath);
  }

 private:
  // White noise through a 2 kHz high-pass, scaled by breath and by the copy's own level, so it never
  // sounds in a gap and stays under the copy at the top of its range.
  void addBreath(float* x, int n, float breath) {
    if (!breathSet_) {
      breathHp_.setHighPass(kBreathHz, kButterworthQ);
      breathSet_ = true;
    }
    Biquad hp = breathHp_;
    EnvFollower env = env_;
    XorShift rng = rng_;
    for (int i = 0; i < n; ++i) {
      const float e = env.run(x[i], kBreathUp, kBreathDown);
      x[i] += breath * e * hp.run(rng.next());
    }
    breathHp_ = hp;
    env_ = env;
    rng_ = rng;
  }

  SpectralTilt tilt_;
  Biquad breathHp_;
  EnvFollower env_;
  XorShift rng_;
  bool breathSet_ = false;
};

}  // namespace crowd_detail
}  // namespace cv
