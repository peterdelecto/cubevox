// The audio block's tail, pure so the host test covers it: the guard in front of the polish stages
// and the output gain ramp.

#pragma once

#include <stdint.h>

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
