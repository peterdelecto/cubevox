// The flow of one audio block after its input is loaded, pure so the host test covers it and the board
// and the host run the same code. It owns the layout fade, the slot crossfades, the output gain ramp,
// the engine clears waiting for a silent block or a chain block, the BYPASS phase, the late-block flag and the scope's
// ring. The board and the host keep the engines, the snapshots, the block buffers and the output sink.

#pragma once

#include <stdint.h>

#include <algorithm>
#include <array>

#include "core/chain_run.h"
#include "core/compose.h"
#include "core/late_block.h"
#include "core/layout_fade.h"
#include "core/output_stage.h"
#include "core/registry.h"
#include "core/scope.h"
#include "engine/common.h"

// Engine clears per silent block. One per block keeps the largest clear inside the block budget.
constexpr int kResetsPerSilentBlock = 1;
static_assert(kResetsPerSilentBlock * (1 + kFadeHoldBlocks) >= kEngineCount,
              "the silent blocks must be enough to clear every engine");

// Engine clears per chain block, for the engines a layout change taken at once leaves waiting. The clear
// lands on top of a full chain pass. The largest engine is Mob's (65,792 B on the host), and the board's
// memset clears 51 kB in 10k to 26k cycles (engine/CLAUDE.md), so one clear costs at most about 34k
// cycles, 5.3 % of the 640,000-cycle block, beside the costliest layout's 44 % worst block.
constexpr int kResetsPerChainBlock = 1;

// Samples over which a late block's start, and the first chain block after it, blend in from the last
// sample sent.
constexpr int kLateBlendSamples = 32;

class BlockFlow {
 public:
  // What the block starts with: the layout fade's step and whether this block passes the dry input in
  // place of the chain, because the last one ran late.
  struct Start {
    LayoutFade::Block fade;
    bool dry;
  };

  // What the block did, for the caller's counters.
  struct Report {
    ChainCycles cycles;       // the stage times, when the chain ran
    bool chainRan = false;    // the chain ran, so the Input gate's detector holds this block's peak
    bool discarded = false;   // a slot produced a NaN or an infinity, so the block was cut
  };

  // Starts a block: moves the layout fade, holding the output at zero while `outputHeld`, and takes the
  // late flag. When start.fade.swap is set the caller adopts the newest snapshot (adopt) before run().
  // While `awaitingSnapshot` (no snapshot has been published yet) the block is silent: it runs no
  // card, so nothing processed with the default settings is left in a delay line or a reverb tail.
  Start begin(bool outputHeld, bool layoutPending, bool awaitingSnapshot = false) {
    fade_.hold(outputHeld);
    Start s{fade_.next(layoutPending), false};
    if (awaitingSnapshot) s.fade.silent = true;
    s.dry = late_.takeDry(s.fade.silent);
    return s;
  }

  // A block counted late. The next one passes the dry input.
  void markLate() { late_.markLate(); }
  uint32_t dryBlocks() const { return late_.dryBlocks(); }
  void clearLateCount() { late_.clearCount(); }

  // Engines whose state clears in the next silent blocks.
  void queueReset(uint32_t engineMask) { resetMask_ |= engineMask; }

  // Puts every slot at the settled end of its toggle, with no crossfade.
  void settleSlots(const std::array<bool, kSlotCount>& on) { slots_.settle(on); }

  // At the zero point of a layout fade, moves from snapshot `from` to `to`. Queues a clear for every
  // engine whose slot changed card and for every engine whose slot sits settled off and turns on, as a
  // plain toggle clears it at the start of its fade in. Then settles every slot, since the output is at
  // zero and nothing needs a crossfade.
  void adopt(const ChainSnapshot& from, const ChainSnapshot& to);

  // For a published snapshot `to` whose layout differs from the running `from`. When the change cannot
  // alter what is heard (changeHeard: every slot that changes card sits off and settled in both, and no
  // live slot plays from an engine that changes or clears), queues the clears of the changed slots' engines
  // and returns true, and the caller runs `to` from the next block with no fade. Chain blocks then clear
  // kResetsPerChainBlock engines each, and a slot that plays from a waiting engine stays at the dry input
  // until it clears. Otherwise returns false and the caller waits for the fade's zero point.
  bool adoptAtOnce(const ChainSnapshot& from, const ChainSnapshot& to);

  // For the host tests: the engines still waiting for a clear, and slot `s`'s fade level.
  uint32_t enginesWaiting() const { return resetMask_; }
  int slotLevel(int s) const { return slots_.level(s); }

  // The scope's ring, which holds the chain output of the newest blocks. The main loop reads it.
  ScopeRing& scope() { return scope_; }
  const ScopeRing& scope() const { return scope_; }

  // Runs one block of cv::kBlock samples. `a` holds the input and takes the output, `b` is scratch,
  // `held` is the snapshot the block runs, and sink(i, v) takes each output sample. The silent
  // blocks of a layout change clear the waiting engines, a late block passes the dry input, and
  // every other block runs the chain, after clearing a waiting engine when one is left. BYPASS mixes the dry input in over a chain block. A NaN or an
  // infinity out of a slot cuts the output, clears every engine in the blocks that follow and
  // resets CLEANUP EQ. Every block writes one block into the scope's ring, the chain output ahead of
  // CLEANUP EQ's back half when the chain ran and zeros when it did not.
  template <typename Clock, typename Sink>
  Report run(ChainEngines& engines, const ChainSnapshot& held, const Start& start, float* a, float* b,
             bool bypassOn, Clock cycleClock, Sink&& sink) {
    constexpr int n = cv::kBlock;
    const float* const dry = input_.data();
    std::copy(a, a + n, input_.begin());
    Report report;
    if (start.fade.silent) {
      // The output is at zero here. The block skips the chain so the engine clears never land on top
      // of a full chain pass.
      resetWaiting(engines, kResetsPerSilentBlock);
      for (int i = 0; i < n; ++i) a[i] = 0.0f;
      scope_.writeSilence();
    } else if (start.dry) {
      // The last block ran late. This one skips the chain and plays the input, by the BYPASS mix
      // settled at full dry. The BYPASS fade itself does not move.
      uint32_t atDry = kBypassFull;
      mixBypass(a, dry, n, true, atDry);
      scope_.writeSilence();
    } else {
      report.chainRan = true;
      clearWhileRunning(engines, held.layout);
      auto tap = [this](const float* block) { scope_.write(block); };
      if (!runChain(engines, slots_, a, b, n, held, cycleClock, report.cycles, tap)) {
        report.discarded = true;
        discard(engines, held);
      }
    }
    if (!start.dry) mixBypass(a, dry, n, bypassOn, bypassPhase_);
    blendJoin(a, start.dry);
    emitOutput(a, n, gain_.next(held.outputGain, start.fade.from, start.fade.to, n), sink);
    return report;
  }

 private:
  // A slot's NaN or infinity was silenced before CLEANUP EQ's back half. The output cuts to zero and every
  // engine clears in the silent blocks that follow, because the one that produced it is not known.
  // CLEANUP EQ clears too, since the cut output leaves its state to restart from rest.
  void discard(ChainEngines& engines, const ChainSnapshot& held);

  // Clears up to `count` of the waiting engines, lowest first.
  void resetWaiting(ChainEngines& engines, int count);

  // Ahead of a chain block: clears kResetsPerChainBlock waiting engines, then holds at the dry input every
  // slot of `layout` that still plays from a waiting one.
  void clearWhileRunning(ChainEngines& engines, const Layout& layout);

  // Eases the block's start from the last sample sent, over kLateBlendSamples, when the block is a
  // dry one or the first after it. Records the block's last sample.
  void blendJoin(float* a, bool dry);

  LayoutFade fade_;
  SlotFades slots_;
  OutputGain gain_;
  LateBlocks late_;
  ScopeRing scope_;
  uint32_t resetMask_ = 0;
  uint32_t bypassPhase_ = 0;  // the BYPASS crossfade position, 0 plays the chain (see mixBypass)
  std::array<float, cv::kBlock> input_{};  // the block's input, which BYPASS and the dry pass read after the chain wrote over `a`
  float lastOut_ = 0.0f;      // the last sample before the output stage
  bool afterDry_ = false;     // the last block was a dry one
};
