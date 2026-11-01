// The audio block's chain walk, the slot toggle crossfade and the BYPASS mix, pure so the host test
// covers them. The board owns the engines, the block buffers and the cycle counter, and hands them in.

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
  cv::Crowd crowd;  // a plain global like the others; its voices read the ring that `pitch` writes
  cv::Lofi lofi;
  cv::Bloom bloom;  // 31 kB, two grain buffers; reads inputGate's detector as its word key
  cv::PolishComp comp;
  cv::Polish polish;
};

// Clears the state of engine `e`. Empty (kNoEngine) clears nothing.
void resetEngine(ChainEngines& engines, Engine e);

// Holds the cycles each stage of one pass took, with one entry per slot and then one each for
// POLISH COMP and POLISH EQ. The EQ entry sums its front half ahead of slot 1 and its back half.
struct ChainCycles {
  std::array<uint32_t, kSlotCount> slot{};
  uint32_t comp = 0;
  uint32_t eq = 0;
};

// Runs the engine of card `c` over `n` samples from `in` into `out`. Returns false when it wrote
// nothing (an Empty slot, or an Autotune, Octave or Formant card after the pitch stage ran), so the
// caller keeps its buffers. Autotune, Octave, Formant and the Crowd card share one PitchFx call,
// made at the first pitch-group card's slot; `pitchDone` carries that across the slots of a pass.
// A Crowd card that makes that call runs its own engine on the pitch stage's output, which is its
// lead. A Bloom card keys on the Input gate's detector when the layout places that card
// (`p.bloom.keyFromGate`), reading `engines.inputGate.open()` as the block stands when Bloom runs.
bool runStage(ChainEngines& engines, Card c, const float* in, float* out, int n, const ChainParams& p,
              bool& pitchDone);

// A quarter turn in the phase units of cv::sinTurns. The BYPASS mix and the slot crossfade both run
// a phase from 0 (the chain or engine side) to this (the dry input).
constexpr uint32_t kBypassFull = 1u << 30;

// A slot's toggle crossfades between the engine and the dry input over this many blocks (6 blocks
// of 64 samples, 8 ms), at equal power, with the same law as the BYPASS mix: the dry gain is the sine
// of the phase and the engine gain its cosine.
constexpr int kSlotFadeBlocks = 6;

// Each slot's fade level in blocks, kept by the audio interrupt. 0 plays the dry input with the
// engine stopped, kSlotFadeBlocks plays the engine alone, and the levels between are a crossfade in
// progress. A block moves a slot one level toward its toggle, so a flip during a fade turns it
// round from where it is.
class SlotFades {
 public:
  // The levels at a block's first and last sample.
  struct Step {
    int from;
    int to;
  };

  // Starts with every slot at the engine.
  SlotFades() { level_.fill(kSlotFadeBlocks); }

  // Puts each slot at the settled end of its toggle: the engine for on, the dry input for off. For a
  // new layout, the first snapshot and a discarded block, where no fade is wanted.
  void settle(const std::array<bool, kSlotCount>& on) {
    for (int s = 0; s < kSlotCount; ++s) level_[static_cast<size_t>(s)] = on[static_cast<size_t>(s)] ? kSlotFadeBlocks : 0;
  }

  // Moves slot `s` one block toward `on` and returns the levels around that block.
  Step step(int s, bool on) {
    int& level = level_[static_cast<size_t>(s)];
    const int from = level;
    if (on && level < kSlotFadeBlocks) ++level;
    if (!on && level > 0) --level;
    return {from, level};
  }

  int level(int s) const { return level_[static_cast<size_t>(s)]; }

 private:
  std::array<int, kSlotCount> level_;
};

// Mixes the dry input `dry` into the engine output in `wet` across one block of `n` samples, with
// the slot's fade moving from level `from` to level `to`. The block's gains run from the phase at
// `from` to the phase at `to` sample by sample, so consecutive blocks join without a step.
void mixSlotFade(float* wet, const float* dry, int n, int from, int to);

// Runs slot `s` for one block from `in` into `out`: the engine of its card with the toggle
// crossfade. A slot settled off runs no engine and writes nothing, so the caller keeps its buffers
// and the dry input passes. A slot that starts a fade in has its engine cleared first. A flip back
// during a fade-out leaves the running engine alone. The Input gate's engine runs while its slot is
// off, because its detector feeds the status reply's `floor_dbfs`. Returns false when it wrote
// nothing. The shared pitch stage runs at the first pitch-group card's slot (Autotune, Octave,
// Formant or Crowd) while any pitch-group toggle is on, and its fade follows all of them. A Formant
// card behind another pitch-group card acts at that first slot, not at its own. A Crowd card keeps a
// live ring and tracker with Autotune and Octave off, wherever it sits against them. The Crowd card's
// own engine runs at its own slot and ramps itself on its own toggle.
bool runSlot(ChainEngines& engines, SlotFades& fades, int s, const ChainSnapshot& snap, const float* in, float* out,
             int n, bool& pitchDone);

// Walks one block through the chain. The front half of POLISH EQ runs first, ahead of slot 1. The
// slots run in order, then the NaN guard, then POLISH COMP and the back half of POLISH EQ. POLISH
// COMP and both EQ halves run always, and each is a bit-exact copy once settled off. `a` holds
// the input and takes the finished block; `b` is scratch. `cycleClock` returns a free-running cycle
// count and times each stage into `cycles`. `fades` carries the slot toggle crossfades from block to
// block. Returns false when a slot produced a NaN or an infinity. The block was then silenced before
// the polish stages, so the caller cuts the output and clears the engines.
template <typename Clock>
bool runChain(ChainEngines& engines, SlotFades& fades, float* a, float* b, int n, const ChainSnapshot& snap,
              Clock cycleClock, ChainCycles& cycles) {
  const ChainParams& p = snap.params;
  float* const result = a;
  bool pitchDone = false;
  const uint32_t frontStart = cycleClock();
  engines.polish.processFront(a, a, n, p.eq);
  const uint32_t frontCycles = cycleClock() - frontStart;
  for (int s = 0; s < kSlotCount; ++s) {
    const uint32_t start = cycleClock();
    const bool wrote = runSlot(engines, fades, s, snap, a, b, n, pitchDone);
    cycles.slot[static_cast<size_t>(s)] = cycleClock() - start;
    if (wrote) std::swap(a, b);
  }
  const bool finite = guardBlock(a, n);
  const uint32_t compStart = cycleClock();
  engines.comp.process(a, b, n, p.comp);
  const uint32_t eqStart = cycleClock();
  engines.polish.processBack(b, a, n, p.eq);
  cycles.comp = eqStart - compStart;
  cycles.eq = frontCycles + (cycleClock() - eqStart);
  if (a != result) {
    for (int i = 0; i < n; ++i) result[i] = a[i];
  }
  return finite;
}

// TOP SWITCH = BYPASS crossfades from the chain output to the dry input and back at equal power. The
// chain keeps running underneath. The phase runs a quarter turn from the chain (0) to the dry input
// (kBypassFull) over kFadeMs, like the layout fade, and the two gains are the cosine and sine of it.
// Settled at either end, the block is the chain output untouched or exactly the dry input.

// Mixes the dry input `dry` into the chain output in `a` for a block of `n` samples, and moves `phase`
// toward the end that `bypassOn` selects. The caller keeps `phase` between blocks and starts it at 0.
void mixBypass(float* a, const float* dry, int n, bool bypassOn, uint32_t& phase);
