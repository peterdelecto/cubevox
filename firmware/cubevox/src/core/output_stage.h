// The audio block's output side, pure so the host test covers it: the guard in front of VOX EQ's
// back half, the output gain ramp and the loop that shapes a block for the DAC.

#pragma once

#include <stdint.h>
#include <string.h>

#include "engine/soft_ceiling.h"

// Limits to full scale. NaN fails both comparisons and maps to 0, which keeps the int32 cast defined.
inline float clampSample(float v) {
  if (v > 1.0f) return 1.0f;
  if (v < -1.0f) return -1.0f;
  return cv::isNaNBits(v) ? 0.0f : v;
}

// One output sample. The soft ceiling shapes the full-scale signal first and `gain` (the output trim
// times the layout fade) scales the result, so a clipped chain keeps its shaped peak at any trim.
// The clamp comes last. At a gain of exactly 1 the result is the ceiling's alone.
inline float shapeOutput(float x, float gain) { return clampSample(cv::softCeiling(x) * gain); }

// Zeroes the block and returns false when it holds a NaN, an infinity or a sample too loud for the
// chain. The bound is a mean square of 1e6, +60 dBFS RMS. Chain samples stay 67 dB under it, so
// nothing else trips it. The test reads the sum of squares through its bits, so a NaN, an
// infinity and a finite runaway all fail, and samples of opposite sign cannot cancel. A block past
// the bound comes from an engine running away, so zeroing it keeps the runaway out of VOX EQ's back
// half, the scope and the output before a sum of squares can overflow a float.
bool guardBlock(float* x, int n);

// The gain at a block's first sample and its step per sample.
struct GainRamp {
  float start;
  float step;
};

// The output gain from block to block. A change of the output trim (OUTPUT PAD OFF / ON is 30 dB)
// ramps across one block instead of stepping between two samples; the layout fade's own ramp
// multiplies in.
class OutputGain {
 public:
  // The ramp for the next block of `n` samples: from where the last block left off to `trim` times
  // the fade level `fadeTo` at the block's end. `fadeFrom` is the fade level at the block's start.
  GainRamp next(float trim, float fadeFrom, float fadeTo, int n);

 private:
  float trim_ = 1.0f;  // the trim the last block ended on; a snapshot starts at unity
};

// Shapes the `n` samples of `a` with shapeOutput along `ramp` and hands each result to sink(i, v).
// A block whose peak sits under the ceiling's knee, on a ramp inside [-1, 1], passes the ceiling and
// the clamp unchanged, so that block skips both. A NaN's bits sort above the knee's, so it takes the
// full path.
template <typename Sink>
void emitOutput(const float* a, int n, GainRamp ramp, Sink&& sink) {
  uint32_t peak = 0;
  for (int i = 0; i < n; ++i) {
    uint32_t bits;
    memcpy(&bits, &a[i], sizeof bits);
    bits &= 0x7FFFFFFFu;
    peak = bits > peak ? bits : peak;
  }
  uint32_t kneeBits;
  memcpy(&kneeBits, &cv::kSoftCeilingKnee, sizeof kneeBits);
  const float last = ramp.start + static_cast<float>(n - 1) * ramp.step;
  const bool unitGain = ramp.start >= -1.0f && ramp.start <= 1.0f && last >= -1.0f && last <= 1.0f;

  float gain = ramp.start;
  if (peak <= kneeBits && unitGain) {
    for (int i = 0; i < n; ++i) {
      sink(i, a[i] * gain);
      gain += ramp.step;
    }
    return;
  }
  for (int i = 0; i < n; ++i) {
    sink(i, shapeOutput(a[i], gain));
    gain += ramp.step;
  }
}
