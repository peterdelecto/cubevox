// Audio path: SAI1 master TX to the PCM5102A and slave RX from the PCM1808 (both I2S, 32-bit
// slots, 48 kHz, MCLK 12.288 MHz on pins::kSaiMclkA). Each 64-frame block runs the engine chain in slot
// order from a low-priority interrupt that the RX DMA half-transfer pends.

#pragma once

#include <Arduino.h>
#include <stdint.h>

#include <array>

#include "board/build_kind.h"
#include "core/chain_params.h"
#include "core/compose.h"

// Cycle buckets: one per slot, then one each for the two polish stages.
constexpr int kStatComp = kSlotCount;
constexpr int kStatEq = kSlotCount + 1;
constexpr int kStatBuckets = kSlotCount + 2;

struct AudioStats {
  uint32_t blocks = 0;        // blocks processed
  uint32_t late = 0;          // blocks that took longer than the budget (the DMA played stale data)
  uint32_t overruns = 0;      // blocks that were not even started before the next one arrived
  uint32_t maxCycles = 0;     // worst block time in CPU cycles
  uint32_t avgCycles = 0;     // mean block time since the last peaks reset
  uint32_t budgetCycles = 0;  // CPU cycles available per block
  std::array<uint32_t, kStatBuckets> slotAvg = {};    // mean cycles per bucket since the reset
  std::array<uint32_t, kStatBuckets> slotWorst = {};  // worst single block per bucket since the reset
};

// Brings up PLL3, SAI1 and DMA, and starts the stream. Returns false on failure; audioReport() says why.
bool audioInit();

// The "[boot] audio ..." line, or the "[ERROR] audio: ..." line when audioInit failed. Also reports
// SAI and DMA errors counted since boot.
void audioReport(Print& out);

// SAI and DMA transfer errors since boot, and the HAL error bits seen.
struct AudioErrors {
  uint32_t count = 0;
  uint32_t codes = 0;
};
AudioErrors audioErrors();

// Hands a complete snapshot to the audio interrupt. Lock-free, call from the main loop. A snapshot with
// the layout the interrupt runs is adopted at once; a different layout fades the output out and in
// around the swap (core/layout_fade).
void audioPublish(const ChainSnapshot& snapshot);

// CPU load over the blocks since the previous call (or since boot), as percent of the block budget,
// and the cumulative late-block count. Resets the window; call from the main loop.
struct AudioLoad {
  float meanPercent = 0.0f;
  float worstPercent = 0.0f;
  uint32_t late = 0;
};
AudioLoad audioTakeLoad();

// Cost of the last layout swap in CPU cycles: the swap block (where the engine clears land) and the
// worst block from the swap to the end of the fade-in. Zero until a swap has happened.
struct AudioSwapStats {
  uint32_t count = 0;
  uint32_t swapBlockCycles = 0;
  uint32_t worstCycles = 0;
};
AudioSwapStats audioSwapStats();

// Blocks processed so far. One word read, cheap enough for every loop pass.
uint32_t audioBlockCount();

AudioStats audioStats();

#if CUBEVOX_BENCH
// Clears late, overruns and maxCycles so a measurement can exclude the first blocks.
void audioResetPeaks();

// Stops the RX DMA so no more blocks arrive, to prove the stall watch.
void audioStopForBench();

// Loops the clip compiled in by CUBEVOX_BENCH_CLIP in place of the SAI input. Returns the
// new state; false when no clip is compiled in.
bool audioBenchClip(bool on);

// True while the clip replaces the SAI input.
bool audioBenchClipOn();
#endif
