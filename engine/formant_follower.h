#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/pitch_fx.h"
#include "engine/smooth.h"

// Yawn: the throat drops on every syllable. The card moves the Formant voice's SHIFT with the singing,
// the way an envelope-filter pedal moves its filter with the pluck. The pitch stays where it is. The card
// drives the Formant voice inside PitchFx with its MIX pinned at 1, so the output is the shifted voice
// alone at the Formant card's trim.
//
// Each call reads the peak of its block and follows it with a 3 ms attack and a 70 ms release. The
// follower in dBFS sets the drive. SENSITIVITY puts the full point at -6 dBFS at 0 % and at -36 dBFS at
// 100 %, and the drive rises from 0 at 12 dB under the full point to 1 at it.
//
// An onset is the drive rising through 0.5 after it sat under 0.2, or the follower rising more than 6 dB
// in about 10 ms. Each onset starts a sweep at the drive, the drive times a ramp that falls from 1 to 0
// over the tuning's decay. A held note then rests at 0 until the next onset, and a new onset restarts the
// ramp at the drive of that moment. While no sweep runs, before the first onset and once the drive has
// fallen to 0, the sweep is the drive itself. SHIFT is RANGE times the sweep, and RANGE runs from 0 down
// to -7 st.
//
// PitchFx takes SHIFT in whole semitones and glides it through its own 20 ms one-pole, the only smoothing
// SHIFT gets. Each call hands it the wanted SHIFT plus what the last call's rounding left over, rounded.
// The whole semitones then average to the wanted SHIFT, and the stage's glide smooths their steps.

namespace cv {

constexpr float kFollowerMinRangeSt = -static_cast<float>(kFormantMaxShiftSt);  // RANGE at the pot's top
constexpr float kFollowerMaxRangeSt = 0.0f;                                      // RANGE at the pot's bottom, no effect
constexpr float kFollowerDefaultRangeSt = -5.0f;
constexpr float kFollowerDefaultSensitivity = 0.4f;
constexpr float kFollowerAttackSec = 0.003f;
constexpr float kFollowerReleaseSec = 0.070f;
constexpr float kFollowerFullDbTop = -6.0f;      // the full point at SENSITIVITY 0 %
constexpr float kFollowerFullDbSpan = 30.0f;     // the full point falls this far by SENSITIVITY 100 %
constexpr float kFollowerWindowDb = 12.0f;       // the drive rises from 0 to 1 over this span under the full point
constexpr float kFollowerLevelFloor = 1e-6f;     // the follower reads no lower than this in dB
constexpr float kFollowerFloorDb = -120.0f;      // kFollowerLevelFloor in dBFS
constexpr float kFollowerOnsetArmBelow = 0.2f;   // the drive arms the next crossing under this
constexpr float kFollowerOnsetFireAt = 0.5f;     // and fires an onset rising through this
constexpr float kFollowerOnsetRiseDb = 6.0f;     // a rise past this over about kFollowerOnsetRiseSec fires one too
constexpr float kFollowerOnsetRiseSec = 0.010f;
constexpr float kFollowerMinDecayMs = 10.0f;
constexpr float kFollowerMaxDecayMs = 2000.0f;
constexpr float kFollowerDefaultDecayMs = 300.0f;

struct FormantFollowerTuning {
  float decayMs = kFollowerDefaultDecayMs;  // a sweep falls from the drive to 0 over this
};

struct FormantFollowerParams {
  bool placed = false;                              // the card is in the layout, so its sweep sets the Formant voice's SHIFT
  float sensitivity = kFollowerDefaultSensitivity;  // knob A (SENSITIVITY), 0..1
  float rangeSt = kFollowerDefaultRangeSt;          // knob B (RANGE), 0 down to -7 st
  FormantFollowerTuning tuning;
};

class FormantFollower {
 public:
  void reset() {
    env_ = 0.0f;
    lagDb_ = kFollowerFloorDb;
    drive_ = 0.0f;
    ramp_ = 0.0f;
    sweep_ = 0.0f;
    sweeping_ = false;
    armed_ = true;
    residue_ = 0.0f;
    last_ = 0.0f;
  }

  // The pitch stage's parameters for one call of n samples from `in`. With the card placed they are `p`
  // with the Formant voice's SHIFT taken from the sweep and its MIX at 1; otherwise they are `p` itself.
  const PitchFxParams& follow(const float* in, int n, const PitchFxParams& p, const FormantFollowerParams& f) {
    if (!f.placed) return p;
    params_ = p;
    params_.formant.shiftSt = shiftFor(in, n, f);
    params_.formant.mix = 1.0f;
    return params_;
  }

  // Steps the follower over n samples from `in` and returns this call's SHIFT in whole semitones.
  int shiftFor(const float* in, int n, const FormantFollowerParams& f) {
    if (n <= 0) return static_cast<int>(last_);
    setCoefs(n);

    // A NaN fails the compare and drops out; an infinity reads as silence.
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
      const float a = fabsf(in[i]);
      peak = a > peak ? a : peak;
    }
    if (!(peak < kHugeLevel)) peak = 0.0f;
    env_ += (peak > env_ ? attack_ : release_) * (peak - env_);
    const float levelDb = dbFast(env_ > kFollowerLevelFloor ? env_ : kFollowerLevelFloor);
    const float bottomDb = kFollowerFullDbTop - kFollowerFullDbSpan * smooth::clamp01(f.sensitivity) - kFollowerWindowDb;
    drive_ = smooth::clamp01((levelDb - bottomDb) * (1.0f / kFollowerWindowDb));
    stepSweep(stepOnset(levelDb, bottomDb), n, f.tuning.decayMs);

    const float range = clampf(f.rangeSt, kFollowerMinRangeSt, kFollowerMaxRangeSt);
    const float want = range * sweep_ + residue_;
    if (!(want > kFollowerMinRangeSt - 1.0f && want < kFollowerMaxRangeSt + 1.0f)) {
      residue_ = 0.0f;
      last_ = 0.0f;
      return 0;
    }
    last_ = clampf(floorf(want + 0.5f), kFollowerMinRangeSt, kFollowerMaxRangeSt);
    residue_ = clampf(want - last_, -0.5f, 0.5f);
    return static_cast<int>(last_);
  }

  // The follower's peak level, linear, for the tests.
  float level() const { return env_; }

  // The drive, 0 to 1, for the tests and the render tool.
  float drive() const { return drive_; }

  // The sweep, 0 to 1; SHIFT is RANGE times it.
  float sweep() const { return sweep_; }

 private:
  static constexpr float kHugeLevel = 1e15f;

  // The one-poles' coefficients for calls of n samples, kept for the last n.
  void setCoefs(int n) {
    if (n == coefN_) return;
    attack_ = smooth::blockCoef(kFollowerAttackSec, n);
    release_ = smooth::blockCoef(kFollowerReleaseSec, n);
    lag_ = smooth::blockCoef(kFollowerOnsetRiseSec, n);
    coefN_ = n;
  }

  // True on an onset. `levelDb` is the follower in dBFS. It and its lagged copy are held at the drive's 0
  // point `bottomDb` from below, so a note out of silence rises from there, and neither a reset nor a
  // SENSITIVITY turn reads as a rise. The lagged level trails a rise of R dB/ms by R times
  // kFollowerOnsetRiseSec, so a gap past kFollowerOnsetRiseDb is a rise past it over about that time.
  bool stepOnset(float levelDb, float bottomDb) {
    const float heardDb = levelDb > bottomDb ? levelDb : bottomDb;
    if (lagDb_ < bottomDb) lagDb_ = bottomDb;
    const bool rose = heardDb - lagDb_ > kFollowerOnsetRiseDb;
    lagDb_ += lag_ * (heardDb - lagDb_);
    const bool crossed = armed_ && drive_ >= kFollowerOnsetFireAt;
    if (crossed) {
      armed_ = false;
    } else if (drive_ < kFollowerOnsetArmBelow) {
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
          static_cast<float>(n) / (clampf(decayMs, kFollowerMinDecayMs, kFollowerMaxDecayMs) * kSmpPerMs);
      ramp_ = ramp_ > fall ? ramp_ - fall : 0.0f;
    }
    if (!(drive_ > 0.0f)) sweeping_ = false;
    sweep_ = sweeping_ ? drive_ * ramp_ : drive_;
  }

  float env_ = 0.0f;                   // the block peak after the attack and release
  float lagDb_ = kFollowerFloorDb;     // the heard level through a kFollowerOnsetRiseSec one-pole
  float drive_ = 0.0f;                 // 0 at kFollowerWindowDb under the full point, 1 at it
  float ramp_ = 0.0f;                  // the running sweep's ramp, 1 at its onset down to 0
  float sweep_ = 0.0f;                 // what SHIFT follows, 0 to 1
  bool sweeping_ = false;              // a sweep runs
  bool armed_ = true;                  // the drive sat under kFollowerOnsetArmBelow since the last crossing
  float residue_ = 0.0f;               // what the last call's rounding left over, in semitones
  float last_ = 0.0f;                  // the last SHIFT handed out
  float attack_ = 1.0f;
  float release_ = 1.0f;
  float lag_ = 1.0f;
  int coefN_ = 0;
  PitchFxParams params_;               // the copy follow() hands the stage
};

}  // namespace cv
