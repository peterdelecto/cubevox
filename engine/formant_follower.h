#pragma once

#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/pitch_fx.h"
#include "engine/smooth.h"

// The Formant follower card: the Formant voice with its SHIFT set by the singer's own loudness. A louder
// note moves the formants by up to DEPTH semitones, up for a positive DEPTH and down for a negative one,
// and the pitch stays where it is. The card drives the Formant voice inside PitchFx, so the voice, its
// trim and its MIX law are the Formant card's own.
//
// Each call reads the RMS of its block and follows it with a 10 ms attack and a 150 ms release. The
// envelope is read in dB against a recent peak that falls 3 dB a second. The drive is 0 at 30 dB under
// that peak and 1 at it, so a soft note sits near 0 and a loud one near 1 at any mic level. The drive
// glides through the 20 ms one-pole, and the wanted SHIFT is DEPTH times the drive.
//
// PitchFx takes SHIFT in whole semitones and glides it through its own 20 ms one-pole. Each call hands it
// the wanted SHIFT plus what the last call's rounding left over, rounded. The whole semitones then
// average to the wanted SHIFT, and the stage's glide smooths their steps to within about 0.06 st.

namespace cv {

constexpr float kFollowerMaxDepthSt = static_cast<float>(kFormantMaxShiftSt);
constexpr float kFollowerDefaultDepthSt = 3.0f;
constexpr float kFollowerAttackSec = 0.010f;
constexpr float kFollowerReleaseSec = 0.150f;
constexpr float kFollowerRangeDb = 30.0f;           // the drive runs from 0 to 1 over this span under the peak
constexpr float kFollowerPeakFallDbPerSec = 3.0f;
constexpr float kFollowerPeakFloorDb = -50.0f;      // the peak falls no lower, so noise after a long rest reads near 0
constexpr float kFollowerLevelFloor = 1e-6f;        // -120 dBFS; the envelope reads no lower in dB

struct FormantFollowerParams {
  bool placed = false;                      // the card is in the layout, so its envelope sets the Formant voice's SHIFT
  float depthSt = kFollowerDefaultDepthSt;  // knob A (DEPTH), -7..+7 st
};

class FormantFollower {
 public:
  void reset() {
    env_ = 0.0f;
    peakDb_ = kFollowerPeakFloorDb;
    drive_ = 0.0f;
    residue_ = 0.0f;
  }

  // The pitch stage's parameters for one call of n samples from `in`. With the card placed they are `p`
  // with the Formant voice's SHIFT taken from the envelope; otherwise they are `p` itself.
  const PitchFxParams& follow(const float* in, int n, const PitchFxParams& p, const FormantFollowerParams& f) {
    if (!f.placed) return p;
    params_ = p;
    params_.formant.shiftSt = shiftFor(in, n, f.depthSt);
    return params_;
  }

  // Steps the envelope over n samples from `in` and returns this call's SHIFT in whole semitones.
  int shiftFor(const float* in, int n, float depthSt) {
    if (n <= 0) return static_cast<int>(last_);
    setCoefs(n);
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += in[i] * in[i];
    const float meanSq = sum / static_cast<float>(n);

    // A NaN or an infinity in the block reads as silence.
    const float level = meanSq < kHugePower ? sqrtf(meanSq) : 0.0f;
    env_ += (level > env_ ? attack_ : release_) * (level - env_);
    const float envDb = dbFast(env_ > kFollowerLevelFloor ? env_ : kFollowerLevelFloor);
    const float fallen = peakDb_ - peakFall_;
    peakDb_ = envDb > fallen ? envDb : (fallen > kFollowerPeakFloorDb ? fallen : kFollowerPeakFloorDb);
    const float drive = smooth::clamp01((envDb - peakDb_ + kFollowerRangeDb) * (1.0f / kFollowerRangeDb));
    drive_ = smooth::step(drive_, drive, glide_);

    const float depth = clampf(depthSt, -kFollowerMaxDepthSt, kFollowerMaxDepthSt);
    const float want = depth * drive_ + residue_;
    if (!(want > -kFollowerMaxDepthSt - 1.0f && want < kFollowerMaxDepthSt + 1.0f)) {
      residue_ = 0.0f;
      last_ = 0.0f;
      return 0;
    }
    last_ = clampf(floorf(want + 0.5f), -kFollowerMaxDepthSt, kFollowerMaxDepthSt);
    residue_ = clampf(want - last_, -0.5f, 0.5f);
    return static_cast<int>(last_);
  }

  // The drive after its glide, 0 to 1, for the tests and the render tool.
  float drive() const { return drive_; }

 private:
  static constexpr float kHugePower = 1e30f;

  // The one-poles' coefficients and the peak's fall for calls of n samples, kept for the last n.
  void setCoefs(int n) {
    if (n == coefN_) return;
    attack_ = smooth::blockCoef(kFollowerAttackSec, n);
    release_ = smooth::blockCoef(kFollowerReleaseSec, n);
    glide_ = smooth::blockCoef(smooth::kSmoothSec, n);
    peakFall_ = kFollowerPeakFallDbPerSec * static_cast<float>(n) / static_cast<float>(kSampleRate);
    coefN_ = n;
  }

  float env_ = 0.0f;                         // the block RMS after the attack and release
  float peakDb_ = kFollowerPeakFloorDb;      // the recent peak of the envelope in dB
  float drive_ = 0.0f;                       // 0 at kFollowerRangeDb under the peak, 1 at it, glided
  float residue_ = 0.0f;                     // what the last call's rounding left over, in semitones
  float last_ = 0.0f;                        // the last SHIFT handed out
  float attack_ = 1.0f;
  float release_ = 1.0f;
  float glide_ = 1.0f;
  float peakFall_ = 0.0f;                    // dB the peak falls over one call
  int coefN_ = 0;
  PitchFxParams params_;                     // the copy follow() hands the stage
};

}  // namespace cv
