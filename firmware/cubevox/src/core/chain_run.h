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
#include "engine/autotune_fx.h"
#include "engine/formant_fx.h"
#include "engine/octave_fx.h"
#include "engine/upstream_pitch.h"

// Gathers the engines the chain runs, each in reset state once constructed. AutotuneFx, OctaveFx, Unison
// and Distortion are references because the board places them in DTCM.
struct ChainEngines {
  ChainEngines(cv::AutotuneFx& autotuneFx, cv::OctaveFx& octaveFx, cv::Unison& unisonFx, cv::Distortion& distortionFx)
      : autotune(autotuneFx), octave(octaveFx), unison(unisonFx), distortion(distortionFx) {}

  cv::AutotuneFx& autotune;  // its tracker also serves the Formant, Crowd and Growl engines behind it
  cv::OctaveFx& octave;
  cv::FormantFx formant;  // a plain global like the others; 26 kB with its ring, its own tracker and two voices
  cv::FormantFollower follower;  // sets the Formant voice's SHIFT ahead of each Formant call while Apraxia is placed
  cv::Gate inputGate;
  cv::Unison& unison;
  cv::Slapback slapback;
  cv::Distortion& distortion;
  cv::Gate gate;
  cv::Reverb reverb;
  cv::Crowd crowd;  // 60 kB: a ring and a tracker of its own, fed by the Autotune slot when one sits ahead
  cv::Lofi lofi;
  cv::Bloom bloom;  // the Chatter card's engine, 46 kB, three slices; reads inputGate's detector as its word key
  cv::Growl growl;  // keeps a 15 kB ring of its own input and a tracker for when no Autotune sits ahead

  cv::Polish polish;  // VOX EQ
};

// Clears the state of engine `e`. Empty (kNoEngine) clears nothing. The Formant engine's reset clears
// the Apraxia follower's envelope too.
void resetEngine(ChainEngines& engines, Engine e);

// Holds the cycles each stage of one pass took, with one entry per slot and one for VOX EQ. The EQ
// entry sums its front half ahead of slot 1 and its back half.
struct ChainCycles {
  std::array<uint32_t, kSlotCount> slot{};
  uint32_t eq = 0;
};

// What one block's walk carries from slot to slot, for the cards that read Autotune's tracker. An Autotune
// card sets it, and the cards behind it hand their share on:
//   1. `autotuneAhead` is true once an Autotune slot has been walked.
//   2. `adjacent` is true while nothing but Empty slots has been walked since it. The Formant engine joins
//      Autotune's grain pass (its tracker and its input) only then.
//   3. `upstream` is what Deepthroat and the Formant engine read: Autotune's tracker result, the shift the
//      slots since have put on the pitch and the delay they have put on the audio. The Octave card adds its
//      interval to the shift while its voice carries half the power, and its latency to the delay. The
//      Formant engine adds the part of its lag that Autotune's latency does not already count: none once
//      Autotune is settled on, since the Formant engine directly behind it cuts Autotune's input and waits
//      for its corrected voice once. `upstream.group` names the Autotune engine for it, with the slot's
//      engine gain.
struct PitchPath {
  bool autotuneAhead = false;
  bool adjacent = false;
  cv::UpstreamPitch upstream;
};

// What the layout asks of a slot's engine beyond its own card.
struct StageContext {
  cv::TrackerNeeds tracker;  // Autotune: what the readers behind it ask of its tracker
  cv::GridMarks groupGrid;   // Autotune: the grain grid of the Formant voice directly behind it, when one runs
  bool feedsFormant = false; // Autotune: a live Formant engine sits directly behind it, so Autotune's input goes to it
  bool feedsCrowd = false;   // Autotune: a Mob card sits behind it, so Autotune's input goes into Mob's ring
  float slotShare = 1.0f;    // the slot's toggle fade, 0 dry .. 1 engine
  float slotGain0 = 1.0f;    // the slot's engine gain at the block's first sample, the S curve of mixSlotFade
  float slotGain1 = 1.0f;    // and at its last
};

// Runs the engine of card `c` over `n` samples from `in` into `out`. Returns false when it wrote
// nothing (an Empty slot), so the caller keeps its buffers. Every card runs its own engine on `in`, the block
// the slot ahead hands it, and updates `path` for the cards behind it. The Formant engine joins Autotune's grain
// pass when `path` says it sits directly behind it, and runs its own tracker and ring otherwise. With Apraxia
// placed, the call takes the Formant voice's SHIFT from the follower's envelope of `in`. A Mob card reads Autotune's
// tracker and correction when one sits ahead, and runs its own tracker on its own ring otherwise. A Deepthroat card
// reads Autotune's tracker with `path.upstream`. A Chatter card keys on the Input gate's detector when the
// layout places that card (`p.bloom.keyFromGate`), reading `engines.inputGate.open()` as the block stands when
// it runs. It cuts its slices from `in`.
bool runStage(ChainEngines& engines, Card c, const float* in, float* out, int n, const ChainParams& p,
              PitchPath& path, const StageContext& ctx = StageContext{});

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
// and the dry input passes. A stopped engine with a ring clears a slice of it each block (its `rest`), so
// the clear at its next start is already done. A slot that starts a fade in has its engine cleared first,
// except Autotune, Octave and the Formant engine, which clear themselves on their first call after a rest.
// A flip back during a fade-out leaves the running engine alone. The Input gate's engine runs while its slot
// is off, because its detector feeds the status reply's `floor_dbfs` and keys the Chatter card; its own off
// fade ends inside the slot fade, so the settled slot passes the input bit for bit.
//
// The Autotune engine runs while its toggle is off whenever a reader behind it is on or fading: the
// Formant engine directly behind it, a Crowd card or a Growl card. It keeps the tracker warm, and the slot
// then passes the dry input, as an Input gate slot does, once the toggle's fade has run. While Crowd sits
// behind it, the Autotune slot also writes its own input into the Crowd card's ring. Returns false when it
// wrote nothing.
bool runSlot(ChainEngines& engines, SlotFades& fades, int s, const ChainSnapshot& snap, const float* in, float* out,
             int n, PitchPath& path);

// The tap runChain calls when its caller wants none.
struct NoTap {
  void operator()(const float*) const {}
};

// Walks one block through the chain. The front half of VOX EQ runs first, ahead of slot 1. The
// slots run in order, then the NaN guard, then the back half of VOX EQ. Both EQ halves run always,
// and each is a bit-exact copy once settled off. `a` holds the input and takes the finished block;
// `b` is scratch. `cycleClock` returns a free-running cycle count and times each stage into `cycles`.
// `fades` carries the slot toggle crossfades from block to block. `tap(block)` sees the block the
// guard passed, ahead of the back half, which is what the scope draws. Returns false when a slot
// produced a NaN, an infinity or a sample past the guard's bound. The block was then silenced before
// the back half, so the caller cuts the output and clears the engines and VOX EQ.
template <typename Clock, typename Tap = NoTap>
bool runChain(ChainEngines& engines, SlotFades& fades, float* a, float* b, int n, const ChainSnapshot& snap,
              Clock cycleClock, ChainCycles& cycles, Tap&& tap = NoTap()) {
  const ChainParams& p = snap.params;
  float* const result = a;
  PitchPath path;
  const uint32_t frontStart = cycleClock();
  engines.polish.processFront(a, a, n, p.eq);
  const uint32_t frontCycles = cycleClock() - frontStart;
  for (int s = 0; s < kSlotCount; ++s) {
    const uint32_t start = cycleClock();
    const bool wrote = runSlot(engines, fades, s, snap, a, b, n, path);
    cycles.slot[static_cast<size_t>(s)] = cycleClock() - start;
    if (wrote) std::swap(a, b);
  }
  const bool finite = guardBlock(a, n);
  tap(a);
  const uint32_t backStart = cycleClock();
  engines.polish.processBack(a, result, n, p.eq);  // runs in place when `a` is `result`
  cycles.eq = frontCycles + (cycleClock() - backStart);
  return finite;
}

// TOP SWITCH = BYPASS crossfades from the chain output to the dry input and back at equal power. The
// chain keeps running underneath. The phase runs a quarter turn from the chain (0) to the dry input
// (kBypassFull) over kFadeMs, like the layout fade, and the two gains are the cosine and sine of it.
// Settled at either end, the block is the chain output untouched or exactly the dry input.

// Mixes the dry input `dry` into the chain output in `a` for a block of `n` samples, and moves `phase`
// toward the end that `bypassOn` selects. The caller keeps `phase` between blocks and starts it at 0.
void mixBypass(float* a, const float* dry, int n, bool bypassOn, uint32_t& phase);
