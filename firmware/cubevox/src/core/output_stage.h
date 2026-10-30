// The audio block's tail, pure so the host test covers it: the guard in front of the polish stages
// and the output gain ramp.

#pragma once

#include <stdint.h>

#include "engine/soft_ceiling.h"

// Limits to full scale. NaN fails both comparisons and maps to 0, which keeps the int32 cast defined.
inline float clampSample(float v) {
  if (v > 1.0f) return 1.0f;
  if (v < -1.0f) return -1.0f;
  return __builtin_isnan(v) ? 0.0f : v;
}

// One output sample. The soft ceiling shapes the full-scale signal first and `gain` (the output trim
// times the layout fade) scales the result, so a clipped chain keeps its shaped peak at any trim.
// The clamp comes last. At a gain of exactly 1 the result is the ceiling's alone.
inline float shapeOutput(float x, float gain) { return clampSample(cv::softCeiling(x) * gain); }

// Zeroes the block and returns false when any sample is a NaN or an infinity, so nothing after it
// sees one. One running sum covers the block, because a NaN or an infinity in any sample makes the
// sum non-finite and chain samples stay far below the float range. Reads the sum's exponent bits,
// so the check holds under -ffinite-math-only too.
bool guardBlock(float* x, int n);

// The gain at a block's first sample and its step per sample.
struct GainRamp {
  float start;
  float step;
};

// The output gain from block to block. A change of the output trim (OUTPUT LINE / MIC is 30 dB)
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
