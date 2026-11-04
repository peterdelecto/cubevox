// The flow of one audio block after its input is loaded, pure so the host test covers it and the board
// and the host run the same code. It owns the layout fade, the slot crossfades, the output gain ramp,
// the engine clears that wait for a silent block, the BYPASS phase and the late-block flag. The board
// and the host keep the engines, the snapshots, the block buffers and the output sink.

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
#include "engine/common.h"

// Engine clears per silent block. One per block keeps the largest clear inside the block budget.
constexpr int kResetsPerSilentBlock = 1;
static_assert(kResetsPerSilentBlock * (1 + kFadeHoldBlocks) >= kEngineCount,
              "the silent blocks must be enough to clear every engine");

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
    float outputEnergy = 0.0f;  // sum of squares of the samples handed to the sink
  };

  // Starts a block: moves the layout fade, holding the output at zero while `outputHeld`, and takes the
  // late flag. When start.fade.swap is set the caller adopts the newest snapshot (queueReset and
  // settleSlots) before run().
  Start begin(bool outputHeld, bool layoutPending) {
    fade_.hold(outputHeld);
    Start s{fade_.next(layoutPending), false};
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

  // Runs one block of cv::kBlock samples. `a` holds the input and takes the output, `b` is scratch,
  // `held` is the snapshot the block runs, and sink(i, v) takes each output sample. The silent
  // blocks of a layout change clear the waiting engines, a late block passes the dry input, and
  // every other block runs the chain. BYPASS mixes the dry input in over a chain block. A NaN or an
  // infinity out of a slot cuts the output, clears every engine in the blocks that follow and
  // resets POLISH COMP and POLISH EQ.
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
    } else if (start.dry) {
      // The last block ran late. This one skips the chain and plays the input, by the BYPASS mix
      // settled at full dry. The BYPASS fade itself does not move.
      uint32_t atDry = kBypassFull;
      mixBypass(a, dry, n, true, atDry);
    } else {
      report.chainRan = true;
      if (!runChain(engines, slots_, a, b, n, held, cycleClock, report.cycles)) {
        report.discarded = true;
        discard(engines, held);
      }
    }
    if (!start.dry) mixBypass(a, dry, n, bypassOn, bypassPhase_);
    blendJoin(a, start.dry);
    report.outputEnergy = emitOutput(a, n, gain_.next(held.outputGain, start.fade.from, start.fade.to, n), sink);
    return report;
  }

 private:
  // A slot's NaN or infinity was silenced before the polish stages. The output cuts to zero and every
  // engine clears in the silent blocks that follow, because the one that produced it is not known.
  // POLISH COMP and POLISH EQ clear too, since the cut output leaves their state to restart from rest.
  void discard(ChainEngines& engines, const ChainSnapshot& held);

  // Clears up to `count` of the waiting engines, lowest first.
  void resetWaiting(ChainEngines& engines, int count);

  // Eases the block's start from the last sample sent, over kLateBlendSamples, when the block is a
  // dry one or the first after it. Records the block's last sample.
  void blendJoin(float* a, bool dry);

  LayoutFade fade_;
  SlotFades slots_;
  OutputGain gain_;
  LateBlocks late_;
  uint32_t resetMask_ = 0;
  uint32_t bypassPhase_ = 0;  // the BYPASS crossfade position, 0 plays the chain (see mixBypass)
  std::array<float, cv::kBlock> input_{};  // the block's input, which BYPASS and the dry pass read after the chain wrote over `a`
  float lastOut_ = 0.0f;      // the last sample before the output stage
  bool afterDry_ = false;     // the last block was a dry one
};
