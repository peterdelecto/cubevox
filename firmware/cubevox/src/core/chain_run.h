// The audio block's chain walk and BYPASS mix, pure so the host test covers them. The board owns the
// engines, the block buffers and the cycle counter, and hands them in.

#pragma once

#include <stdint.h>

#include <array>
#include <utility>

#include "core/compose.h"
#include "core/output_stage.h"
#include "core/registry.h"
#include "core/slots.h"

// Gathers the engines the chain runs, each in reset state once constructed. PitchFx is a reference
// because the board places it in DTCM.
struct ChainEngines {
  explicit ChainEngines(cv::PitchFx& pitchFx) : pitch(pitchFx) {}

  cv::PitchFx& pitch;
  cv::Gate inputGate;
  cv::Unison unison;
  cv::Slapback slapback;
  cv::Distortion distortion;
  cv::Gate gate;
  cv::Reverb reverb;
  cv::PolishComp comp;
  cv::Polish polish;
};

// Holds the cycles each stage of one pass took, with one entry per slot and then one each for
// POLISH COMP and POLISH EQ.
struct ChainCycles {
  std::array<uint32_t, kSlotCount> slot{};
  uint32_t comp = 0;
  uint32_t eq = 0;
};

// Runs the engine of card `c` over `n` samples from `in` into `out`. Returns false when it wrote
// nothing (an Empty slot, or a pitch card after the first), so the caller keeps its buffers. Autotune
// and Octave share one PitchFx call, made at the first pitch card's slot; `pitchDone` carries that
// across the slots of a pass.
bool runStage(ChainEngines& engines, Card c, const float* in, float* out, int n, const ChainParams& p,
              bool& pitchDone);

// Walks one block through the chain. The slots run in order, then the NaN guard, then POLISH COMP and
// POLISH EQ. Both polish stages run always, and each is a bit-exact copy once settled off. `a` holds
// the input and takes the finished block; `b` is scratch. `cycleClock` returns a free-running cycle
// count and times each stage into `cycles`. Returns false when a slot produced a NaN or an infinity.
// The block was then silenced before the polish stages, so the caller cuts the output and clears the
// engines.
template <typename Clock>
bool runChain(ChainEngines& engines, float* a, float* b, int n, const ChainSnapshot& snap, Clock cycleClock,
              ChainCycles& cycles) {
  const ChainParams& p = snap.params;
  float* const result = a;
  bool pitchDone = false;
  for (int s = 0; s < kSlotCount; ++s) {
    const uint32_t start = cycleClock();
    const bool wrote = runStage(engines, snap.layout.card[s], a, b, n, p, pitchDone);
    cycles.slot[s] = cycleClock() - start;
    if (wrote) std::swap(a, b);
  }
  const bool finite = guardBlock(a, n);
  const uint32_t compStart = cycleClock();
  engines.comp.process(a, b, n, p.comp);
  const uint32_t eqStart = cycleClock();
  engines.polish.process(b, a, n, p.eq);
  cycles.comp = eqStart - compStart;
  cycles.eq = cycleClock() - eqStart;
  if (a != result) {
    for (int i = 0; i < n; ++i) result[i] = a[i];
  }
  return finite;
}

// TOP SWITCH = BYPASS crossfades from the chain output to the dry input and back at equal power. The
// chain keeps running underneath. The phase runs a quarter turn from the chain (0) to the dry input
// (kBypassFull) over kFadeMs, like the layout fade, and the two gains are the cosine and sine of it.
// Settled at either end, the block is the chain output untouched or exactly the dry input.
constexpr uint32_t kBypassFull = 1u << 30;

// Mixes the dry input `dry` into the chain output in `a` for a block of `n` samples, and moves `phase`
// toward the end that `bypassOn` selects. The caller keeps `phase` between blocks and starts it at 0.
void mixBypass(float* a, const float* dry, int n, bool bypassOn, uint32_t& phase);
