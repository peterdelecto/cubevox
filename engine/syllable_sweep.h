#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// The syllable sweep, the law Apraxia and Growl share. It reads the peak of each block of its input and
// follows it with a 3 ms attack and a 70 ms release. The follower in dBFS sets the drive. SENSITIVITY puts
// the full point at -6 dBFS at 0 % and at -36 dBFS at 100 %, and the drive rises from 0 at 12 dB under
// the full point to 1 at it.
//
// An onset is the drive rising through 0.5 after it sat under 0.2, or the follower rising more than 6 dB
// in about 10 ms. Each onset starts a sweep at the drive, the drive times a ramp that falls from 1 to 0
// over DECAY. A held note then rests at 0 until the next onset, and a new onset restarts the ramp at the
// drive of that moment. While no sweep runs, before the first onset and once the drive has fallen to 0,
// the sweep is the drive itself.
//
// The sweep steps once per call. It runs no per-sample work beyond the peak scan.

namespace cv {

constexpr float kSweepDefaultSensitivity = 0.4f;
constexpr float kSweepAttackSec = 0.003f;
constexpr float kSweepReleaseSec = 0.070f;
constexpr float kSweepFullDbTop = -6.0f;      // the full point at SENSITIVITY 0 %
constexpr float kSweepFullDbSpan = 30.0f;     // the full point falls this far by SENSITIVITY 100 %
constexpr float kSweepWindowDb = 12.0f;       // the drive rises from 0 to 1 over this span under the full point
constexpr float kSweepLevelFloor = 1e-6f;     // the follower reads no lower than this in dB
constexpr float kSweepFloorDb = -120.0f;      // kSweepLevelFloor in dBFS
constexpr float kSweepOnsetArmBelow = 0.2f;   // the drive arms the next crossing under this
constexpr float kSweepOnsetFireAt = 0.5f;     // and fires an onset rising through this
constexpr float kSweepOnsetRiseDb = 6.0f;     // a rise past this over about kSweepOnsetRiseSec fires one too
constexpr float kSweepOnsetRiseSec = 0.010f;
constexpr float kSweepMinDecayMs = 10.0f;
constexpr float kSweepMaxDecayMs = 2000.0f;
constexpr float kSweepDefaultDecayMs = 300.0f;

class SyllableSweep {
 public:
  void reset() {
    env_ = 0.0f;
    lagDb_ = kSweepFloorDb;
    drive_ = 0.0f;
    ramp_ = 0.0f;
    sweep_ = 0.0f;
    sweeping_ = false;
    armed_ = true;
  }

  // Steps over n samples from `in` and returns the sweep, 0 to 1. A call of n <= 0 changes nothing.
  float step(const float* in, int n, float sensitivity, float decayMs) {
    if (n <= 0) return sweep_;
    setCoefs(n);

    // A NaN fails the compare and drops out; an infinity reads as silence.
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
      const float a = fabsf(in[i]);
      peak = a > peak ? a : peak;
    }
    if (!(peak < kHugeLevel)) peak = 0.0f;
    env_ += (peak > env_ ? attack_ : release_) * (peak - env_);
    const float levelDb = dbFast(env_ > kSweepLevelFloor ? env_ : kSweepLevelFloor);
    const float bottomDb = kSweepFullDbTop - kSweepFullDbSpan * smooth::clamp01(sensitivity) - kSweepWindowDb;
    drive_ = smooth::clamp01((levelDb - bottomDb) * (1.0f / kSweepWindowDb));
    stepSweep(stepOnset(levelDb, bottomDb), n, decayMs);
    return sweep_;
  }

  // The follower's peak level, linear, for the tests.
  float level() const { return env_; }

  // The drive, 0 to 1, for the tests and the render tool.
  float drive() const { return drive_; }

  // The sweep, 0 to 1.
  float sweep() const { return sweep_; }

 private:
  static constexpr float kHugeLevel = 1e15f;

  // The one-poles' coefficients for calls of n samples, kept for the last n.
  void setCoefs(int n) {
    if (n == coefN_) return;
    attack_ = smooth::blockCoef(kSweepAttackSec, n);
    release_ = smooth::blockCoef(kSweepReleaseSec, n);
    lag_ = smooth::blockCoef(kSweepOnsetRiseSec, n);
    coefN_ = n;
  }

  // True on an onset. `levelDb` is the follower in dBFS. It and its lagged copy are held at the drive's 0
  // point `bottomDb` from below, so a note out of silence rises from there, and neither a reset nor a
  // SENSITIVITY turn reads as a rise. The lagged level trails a rise of R dB/ms by R times
  // kSweepOnsetRiseSec, so a gap past kSweepOnsetRiseDb is a rise past it over about that time.
  bool stepOnset(float levelDb, float bottomDb) {
    const float heardDb = levelDb > bottomDb ? levelDb : bottomDb;
    if (lagDb_ < bottomDb) lagDb_ = bottomDb;
    const bool rose = heardDb - lagDb_ > kSweepOnsetRiseDb;
    lagDb_ += lag_ * (heardDb - lagDb_);
    const bool crossed = armed_ && drive_ >= kSweepOnsetFireAt;
    if (crossed) {
      armed_ = false;
    } else if (drive_ < kSweepOnsetArmBelow) {
      armed_ = true;
    }
    return rose || crossed;
  }

  // Starts a sweep on an onset and steps the running one down its ramp.
  void stepSweep(bool onset, int n, float decayMs) {
    if (onset) {
      sweeping_ = true;
      ramp_ = 1.0f;
    } else if (sweeping_) {
      const float fall =
          static_cast<float>(n) / (clampf(decayMs, kSweepMinDecayMs, kSweepMaxDecayMs) * kSmpPerMs);
      ramp_ = ramp_ > fall ? ramp_ - fall : 0.0f;
    }
    if (!(drive_ > 0.0f)) sweeping_ = false;
    sweep_ = sweeping_ ? drive_ * ramp_ : drive_;
  }

  float env_ = 0.0f;                 // the block peak after the attack and release
  float lagDb_ = kSweepFloorDb;      // the heard level through a kSweepOnsetRiseSec one-pole
  float drive_ = 0.0f;               // 0 at kSweepWindowDb under the full point, 1 at it
  float ramp_ = 0.0f;                // the running sweep's ramp, 1 at its onset down to 0
  float sweep_ = 0.0f;               // 0 to 1
  bool sweeping_ = false;            // a sweep runs
  bool armed_ = true;                // the drive sat under kSweepOnsetArmBelow since the last crossing
  float attack_ = 1.0f;
  float release_ = 1.0f;
  float lag_ = 1.0f;
  int coefN_ = 0;
};

}  // namespace cv
