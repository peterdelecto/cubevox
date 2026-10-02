#pragma once

// Stage feedback simulator. Prototype test signal only, never part of the firmware.
//
// Models a monitor wedge in a closed loop with the box. The box output feeds
// a delayed, band-limited, resonant path and the result is added to the mic
// input on later samples. Per sample:
//
//   r = returnSample(mic);  in = mic + r;  ... box ...;  push(out);
//
// The loop delay is at least 4 ms, so a caller may fetch a block of returns
// first and push the block's outputs afterwards (block <= 192 samples).
//
// Amount sets the wedge path's loop gain with the box bypassed (unity), from
// -30 dB to +1.5 dB re unity. A clean box stays below the edge at the default
// Amount. Gain stages such as DRIVE add small-signal gain on top and tip the loop
// over, which is the effect this models.
//
// Singing ducks the loop (head on the mic blocks the wedge path). An envelope
// follower on the dry input lowers the loop gain by up to kDuckMaxDb.

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"

namespace cv {
namespace stagefb {

constexpr float kPi = 3.14159265358979f;
constexpr float kReturnPeak = 0.25118864f;  // -12 dBFS, hard ceiling on the return
constexpr float kRoomNoiseRms = 3.1622777e-4f;  // -70 dBFS
constexpr int kRingSize = 1024;                 // power of two, > longest delay
constexpr int kMaxModes = 3;
constexpr int kControlEvery = 32;  // samples between coefficient updates
constexpr int kCrossfadeSamples = kSampleRate / 20;  // 50 ms
constexpr float kDelayMinMs = 4.0f;
constexpr float kDelayMaxMs = 12.0f;
constexpr float kModeMinHz = 300.0f;
constexpr float kModeMaxHz = 3500.0f;
constexpr float kQMin = 8.0f;
constexpr float kQMax = 20.0f;
constexpr float kHpHz = 150.0f;
constexpr float kLpHz = 6000.0f;
constexpr float kDriftMax = 0.02f;     // +-2 % of mode frequency
constexpr float kLoopGainMinDb = -30.0f;  // Amount 0
constexpr float kLoopGainMaxDb = 1.5f;    // Amount 100 %
constexpr float kWanderDb = 3.0f;         // slow gain wander, +- around the Amount gain
constexpr float kDuckMaxDb = 4.0f;        // loop gain cut while the vocal is present
constexpr float kDuckAttackSec = 0.015f;
constexpr float kDuckReleaseSec = 0.300f;
constexpr float kDuckLoDb = -50.0f;       // envelope level where ducking starts
constexpr float kDuckHiDb = -30.0f;       // envelope level of full ducking

// TPT state-variable filter; stable under per-block coefficient changes.
struct Svf {
  float ic1 = 0.0f;
  float ic2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  float a3 = 0.0f;
  float k = 1.0f;

  void set(float hz, float q) {
    const float g = tanf(kPi * hz / static_cast<float>(kSampleRate));
    k = 1.0f / q;
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  void clear() { ic1 = ic2 = 0.0f; }

  // Writes the band-pass (unity peak), low-pass and high-pass outputs.
  void tick(float x, float& bp, float& lp, float& hp) {
    const float v3 = x - ic2;
    const float v1 = a1 * ic1 + a2 * v3;
    const float v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    bp = k * v1;
    lp = v2;
    hp = x - k * v1 - v2;
  }
};

class StageFeedback {
 public:
  StageFeedback() { reset(1u); }

  // Applies the controls. Turning on starts from a fresh deterministic state;
  // turning off clears everything and makes the return exactly zero.
  void configure(bool on, float amount01, float movement01) {
    amount_ = clamp01(amount01);
    movement_ = clamp01(movement01);
    if (on && !on_) reset(seed_);
    if (!on && on_) clearState();
    on_ = on;
  }

  bool enabled() const { return on_; }

  // Deterministic restart. Keeps the enabled flag.
  void reset(uint32_t seed) {
    seed_ = seed ? seed : 1u;
    clearState();
    rng_ = seed_ * 2654435761u + 0x9E3779B9u;
    if (rng_ == 0) rng_ = 1u;
    randomizeVoice(0);
    cur_ = 0;
    weight_ = {1.0f, 0.0f};
    walkU_ = uniform();
    walkTarget_ = uniform();
    walkCountdown_ = nextWalkInterval();
    jumpCountdown_ = nextJumpInterval();
    controlTick();
  }

  // Next return sample. Call once per input sample, before the matching push().
  float returnSample(float dryInput) {
    if (!on_) return 0.0f;

    const float mag = fabsf(dryInput);
    env_ += (mag > env_ ? duckAttack_ : duckRelease_) * (mag - env_);

    if (--jumpCountdown_ <= 0) startJump();
    if (--walkCountdown_ <= 0) {
      walkTarget_ = uniform();
      walkCountdown_ = nextWalkInterval();
    }
    if (++controlPhase_ >= kControlEvery) {
      controlPhase_ = 0;
      controlTick();
    }
    stepCrossfade();

    float sum = 0.0f;
    for (int v = 0; v < 2; ++v) {
      if (weight_[v] <= 0.0f) continue;
      sum += weight_[v] * runVoice(voice_[v]);
    }
    ++readPos_;

    float r = sum * gain_ * duckGain() + roomNoise();
    r = kReturnPeak * tanhf(r / kReturnPeak);
    if (!std::isfinite(r)) {
      clearState();
      return 0.0f;
    }
    return r;
  }

  // Box output for one sample.
  void push(float boxOutput) {
    if (!on_) return;
    if (!std::isfinite(boxOutput)) boxOutput = 0.0f;
    if (boxOutput > 4.0f) boxOutput = 4.0f;
    if (boxOutput < -4.0f) boxOutput = -4.0f;
    ring_[static_cast<size_t>(writePos_ & (kRingSize - 1))] = boxOutput;
    ++writePos_;
  }

  // Test hook: current frequency of mode i of the voice carrying the loop.
  float modeHz(int i) const {
    const Voice& v = voice_[cur_];
    return v.baseHz[static_cast<size_t>(i)] * (1.0f + v.drift[static_cast<size_t>(i)]);
  }
  int delaySamples() const { return voice_[cur_].delay; }

 private:
  struct Voice {
    int delay = 192;
    int modes = 2;
    std::array<float, kMaxModes> baseHz{};
    std::array<float, kMaxModes> q{};
    std::array<float, kMaxModes> drift{};
    std::array<float, kMaxModes> driftTarget{};
    std::array<Svf, kMaxModes> mode{};
    Svf hp;
    Svf lp;
  };

  static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

  uint32_t next() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
  }
  float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
  float range(float lo, float hi) { return lo + (hi - lo) * uniform(); }

  float roomNoise() {
    // Uniform noise scaled to the target RMS (uniform RMS = peak / sqrt 3).
    return (2.0f * uniform() - 1.0f) * kRoomNoiseRms * 1.7320508f;
  }

  // Smoothstep from kDuckLoDb to kDuckHiDb on the dry envelope.
  float duckGain() const {
    const float db = 20.0f * log10f(env_ + 1e-9f);
    float t = (db - kDuckLoDb) / (kDuckHiDb - kDuckLoDb);
    t = clamp01(t);
    t = t * t * (3.0f - 2.0f * t);
    return powf(10.0f, -kDuckMaxDb * t / 20.0f);
  }

  void clearState() {
    env_ = 0.0f;
    ring_.fill(0.0f);
    readPos_ = 0;
    writePos_ = 0;
    controlPhase_ = 0;
    for (Voice& v : voice_) {
      v.hp.clear();
      v.lp.clear();
      for (Svf& m : v.mode) m.clear();
    }
  }

  void randomizeVoice(int idx) {
    Voice& v = voice_[static_cast<size_t>(idx)];
    v.delay = static_cast<int>(range(kDelayMinMs, kDelayMaxMs) * 0.001f * kSampleRate);
    v.modes = 2 + static_cast<int>(next() % 2u);
    // Log-uniform modes, kept at least 25 % apart so the peaks stay distinct.
    const float lo = logf(kModeMinHz);
    const float hi = logf(kModeMaxHz);
    for (int i = 0; i < v.modes; ++i) {
      float hz = 0.0f;
      for (int tries = 0; tries < 16; ++tries) {
        hz = expf(range(lo, hi));
        bool apart = true;
        for (int j = 0; j < i; ++j) {
          const float ratio = hz / v.baseHz[static_cast<size_t>(j)];
          if (ratio < 1.25f && ratio > 0.8f) apart = false;
        }
        if (apart) break;
      }
      const size_t s = static_cast<size_t>(i);
      v.baseHz[s] = hz;
      v.q[s] = range(kQMin, kQMax);
      v.drift[s] = range(-kDriftMax, kDriftMax);
      v.driftTarget[s] = v.drift[s];
    }
    v.hp.clear();
    v.lp.clear();
    for (Svf& m : v.mode) m.clear();
    v.hp.set(kHpHz, 0.70710678f);
    v.lp.set(kLpHz, 0.70710678f);
    for (int i = 0; i < v.modes; ++i) {
      const size_t s = static_cast<size_t>(i);
      v.mode[s].set(v.baseHz[s] * (1.0f + v.drift[s]), v.q[s]);
    }
  }

  float runVoice(Voice& v) {
    const float x = ring_[static_cast<size_t>((readPos_ - v.delay) & (kRingSize - 1))];
    float bp = 0.0f;
    float lp = 0.0f;
    float hp = 0.0f;
    v.hp.tick(x, bp, lp, hp);
    v.lp.tick(hp, bp, lp, hp);
    const float band = lp;
    float y = 0.0f;
    for (int i = 0; i < v.modes; ++i) {
      float b = 0.0f;
      float l = 0.0f;
      float h = 0.0f;
      v.mode[static_cast<size_t>(i)].tick(band, b, l, h);
      y += b;
    }
    return y;
  }

  // Mean jump interval runs from 15 s (Movement 0) to 2 s (Movement 1).
  int nextJumpInterval() {
    const float mean = 15.0f - 13.0f * movement_;
    return static_cast<int>(mean * range(0.5f, 1.5f) * kSampleRate);
  }
  int nextWalkInterval() { return static_cast<int>(range(1.0f, 5.0f) * kSampleRate); }

  void startJump() {
    const int other = 1 - cur_;
    randomizeVoice(other);
    cur_ = other;
    jumpCountdown_ = nextJumpInterval();
  }

  void stepCrossfade() {
    constexpr float kStep = 1.0f / static_cast<float>(kCrossfadeSamples);
    for (int v = 0; v < 2; ++v) {
      const float target = (v == cur_) ? 1.0f : 0.0f;
      float& w = weight_[static_cast<size_t>(v)];
      if (w < target) w = (w + kStep > target) ? target : w + kStep;
      else if (w > target) w = (w - kStep < target) ? target : w - kStep;
    }
  }

  // Slow control: gain walk, mode drift, filter coefficients.
  void controlTick() {
    constexpr float kDt = static_cast<float>(kControlEvery) / static_cast<float>(kSampleRate);
    walkU_ += (1.0f - expf(-kDt / 0.5f)) * (walkTarget_ - walkU_);
    const float centerDb = kLoopGainMinDb + (kLoopGainMaxDb - kLoopGainMinDb) * amount_;
    const float db = centerDb + kWanderDb * (2.0f * walkU_ - 1.0f);
    gain_ = powf(10.0f, db / 20.0f);

    const float driftCoef = 1.0f - expf(-kDt / 1.0f);
    for (Voice& v : voice_) {
      for (int i = 0; i < v.modes; ++i) {
        const size_t s = static_cast<size_t>(i);
        if (uniform() < kDt / 2.0f) v.driftTarget[s] = range(-kDriftMax, kDriftMax);
        v.drift[s] += driftCoef * (v.driftTarget[s] - v.drift[s]);
        v.mode[s].set(v.baseHz[s] * (1.0f + v.drift[s]), v.q[s]);
      }
    }
  }

  bool on_ = false;
  float amount_ = 0.5f;
  float movement_ = 0.4f;
  uint32_t seed_ = 1u;
  uint32_t rng_ = 1u;

  std::array<float, kRingSize> ring_{};
  long readPos_ = 0;
  long writePos_ = 0;

  std::array<Voice, 2> voice_{};
  std::array<float, 2> weight_{1.0f, 0.0f};
  int cur_ = 0;
  int controlPhase_ = 0;
  int jumpCountdown_ = 0;
  int walkCountdown_ = 0;
  float walkU_ = 0.5f;
  float walkTarget_ = 0.5f;
  float gain_ = 0.0f;
  float env_ = 0.0f;
  float duckAttack_ = 1.0f - expf(-1.0f / (kDuckAttackSec * kSampleRate));
  float duckRelease_ = 1.0f - expf(-1.0f / (kDuckReleaseSec * kSampleRate));
};

}  // namespace stagefb
}  // namespace cv
