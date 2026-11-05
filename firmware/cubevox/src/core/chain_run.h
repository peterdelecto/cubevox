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

// Gathers the engines the chain runs, each in reset state once constructed. PitchFx, Unison and Distortion
// are references because the board places them in DTCM.
struct ChainEngines {
  ChainEngines(cv::PitchFx& pitchFx, cv::Unison& unisonFx, cv::Distortion& distortionFx)
      : pitch(pitchFx), unison(unisonFx), distortion(distortionFx) {}

  cv::PitchFx& pitch;
  cv::FormantFollower follower;  // sets the Formant voice's SHIFT ahead of each pitch call while its card is placed
  cv::Gate inputGate;
  cv::Unison& unison;
  cv::Slapback slapback;
  cv::Distortion& distortion;
  cv::Gate gate;
  cv::Reverb reverb;
  cv::Crowd crowd;  // a plain global like the others; its voices read the ring that `pitch` writes
  cv::Lofi lofi;
  cv::Bloom bloom;  // the Chatter card's engine, 46 kB, three slices; reads inputGate's detector as its word key
  cv::Growl growl;  // a plain global like crowd; it keeps a 15 kB ring of its own input and reads the tracker that `pitch` runs
  cv::RingMod ringMod;  // reads the tracker that `pitch` runs
  cv::Strings strings;  // the Sympath card: two banks of eight delay lines and its tracker, in D1 with the rest

  // The signal the Chatter card cuts its slices from. captureBloomSource copies the block leaving the
  // tap slot into `bloomSrc` and sets `bloomSrcLive`; `bloomLead` asks a Crowd card that makes the pitch
  // call to copy the stage's output there ahead of its own voices. The card reads its own input while
  // neither is set.
  std::array<float, cv::kBlock> bloomSrc{};
  bool bloomSrcLive = false;
  bool bloomLead = false;

  cv::PolishComp comp;
  cv::Polish polish;
};

// Copies the `n` samples of `block` into the Chatter card's source buffer and marks the buffer live.
void captureBloomSource(ChainEngines& engines, const float* block, int n);

// Where the Chatter card's slices come from: the slot whose output is copied, and whether the layout
// places an Autotune, Octave or Formant card or the Formant follower. The slot is the last one holding such a card, or slot 1
// when the layout has none. It is -1 when the layout places no Chatter card or places it at or ahead of
// that slot, and the card then cuts from its own input.
struct BloomTap {
  int slot;
  bool pitchCard;
};
BloomTap bloomTap(const Layout& layout);

// Clears the state of engine `e`. Empty (kNoEngine) clears nothing. The pitch engine's reset clears
// the Formant follower's envelope too.
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
// caller keeps its buffers. Autotune, Octave, Formant, the Formant follower and the Crowd card share
// one PitchFx call, made at the first pitch-group card's slot; `pitchDone` carries that across the
// slots of a pass. With the Formant follower placed, the call takes the Formant voice's SHIFT from
// the follower's envelope of the call's input.
// A Crowd card that makes that call runs its own engine on the pitch stage's output, which is its
// lead, and so does a Growl card. A Chatter card keys on the Input gate's detector when the layout places that card
// (`p.bloom.keyFromGate`), reading `engines.inputGate.open()` as the block stands when it runs.
// It cuts its slices from `engines.bloomSrc` while `bloomSrcLive` is set, and from `in` otherwise.
bool runStage(ChainEngines& engines, Card c, const float* in, float* out, int n, const ChainParams& p,
              bool& pitchDone);

// A quarter turn in the phase units of cv::sinTurns. The BYPASS mix runs a phase from 0 (the chain
// side) to this (the dry input).
constexpr uint32_t kBypassFull = 1u << 30;

// A slot's toggle crossfades between the engine and the dry input over this many blocks (6 blocks
// of 64 samples, 8 ms). The engine gain follows an S curve, flat at both ends so the blend does not
// bend where the fade starts and stops, and the dry gain is one minus it, so the two sum to 1. An
// engine output that already holds the dry would gain 3 dB under an equal-power fade.
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
// and the dry input passes. A stopped pitch stage clears a slice of its input ring each block (`PitchFx::rest`),
// so the clear at its next start is already done. A slot that starts a fade in has its engine cleared first. A flip back
// during a fade-out leaves the running engine alone. The Input gate's engine runs while its slot is
// off, because its detector feeds the status reply's `floor_dbfs` and keys the Chatter card; its own
// off fade ends inside the slot fade, so the settled slot passes the input bit for bit. Returns false
// when it wrote nothing. The shared pitch stage runs at the first pitch-group card's slot (Autotune, Octave,
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
// block. A layout that places the Chatter card after its tap slot (`bloomTap`) has the block leaving
// that slot copied for its slices, and no copy is made otherwise. Returns false when a slot produced a NaN,
// an infinity or a sample past the guard's bound. The block was then silenced before the polish stages, so the
// caller cuts the output and clears the engines, POLISH COMP and POLISH EQ.
template <typename Clock>
bool runChain(ChainEngines& engines, SlotFades& fades, float* a, float* b, int n, const ChainSnapshot& snap,
              Clock cycleClock, ChainCycles& cycles) {
  const ChainParams& p = snap.params;
  float* const result = a;
  bool pitchDone = false;
  const BloomTap tap = n <= cv::kBlock ? bloomTap(snap.layout) : BloomTap{-1, false};
  engines.bloomSrcLive = false;
  engines.bloomLead = tap.slot >= 0 && tap.pitchCard;
  const uint32_t frontStart = cycleClock();
  engines.polish.processFront(a, a, n, p.eq);
  const uint32_t frontCycles = cycleClock() - frontStart;
  for (int s = 0; s < kSlotCount; ++s) {
    const uint32_t start = cycleClock();
    const bool wrote = runSlot(engines, fades, s, snap, a, b, n, pitchDone);
    cycles.slot[static_cast<size_t>(s)] = cycleClock() - start;
    if (wrote) std::swap(a, b);
    if (s == tap.slot && !engines.bloomSrcLive) captureBloomSource(engines, a, n);
  }
  engines.bloomSrcLive = false;
  engines.bloomLead = false;
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
