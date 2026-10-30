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
#include "core/level.h"

// Cycle buckets: one per slot, then one each for the two polish stages and the input conditioning.
constexpr int kStatComp = kSlotCount;
constexpr int kStatEq = kSlotCount + 1;
constexpr int kStatCond = kSlotCount + 2;
constexpr int kStatBuckets = kSlotCount + 3;

struct AudioStats {
  uint32_t blocks = 0;        // blocks processed
  uint32_t late = 0;          // blocks past the budget, or written after the DMA began reading their half
  uint32_t overruns = 0;      // blocks that were not even started before the next one arrived
  uint32_t maxCycles = 0;     // worst block time in CPU cycles
  uint32_t avgCycles = 0;     // mean block time since the last peaks reset
  uint32_t budgetCycles = 0;  // CPU cycles available per block
  std::array<uint32_t, kStatBuckets> slotAvg = {};    // mean cycles per bucket since the reset
  std::array<uint32_t, kStatBuckets> slotWorst = {};  // worst single block per bucket since the reset
};

// Brings up PLL3, SAI1 and DMA, and starts the stream. Returns false on failure; audioReport() says why.
bool audioInit();

// What audioPoll did this pass.
enum class AudioEvent : uint8_t {
  None,
  Restarted,  // a fault stopped the stream and it runs again; the mute releases after the settle
  Latched,    // a fault past the restart budget, or a restart that failed: muted until reset
};

// Call every loop pass. A fault that stopped the stream (a DMA error, a SAI frame-sync or clock
// error) or kept the block interrupt a ring behind holds the mute; once the output is fully muted
// the stream restarts, at most three times in any minute. Prints one line per restart or latch.
AudioEvent audioPoll(uint32_t nowMs);

// The "[boot] audio ..." line, or the "[ERROR] audio: ..." line when audioInit failed. Also reports
// the SAI and DMA errors, overruns, restarts and NaN blocks counted since boot, and a latched fault.
void audioReport(Print& out);

// SAI and DMA errors since boot, and the HAL error bits seen.
struct AudioErrors {
  uint32_t count = 0;
  uint32_t codes = 0;
};
AudioErrors audioErrors();

// The audio fault counters for status and the boot report.
struct AudioHealth {
  AudioErrors errors;
  uint32_t overruns = 0;   // blocks never started before their half came round again
  uint32_t restarts = 0;   // stream restarts after a fault
  uint32_t nanBlocks = 0;  // blocks discarded before the polish stages for a NaN or an infinity
  bool latched = false;    // a fault past the restart budget, or a failed restart, latched the mute
};
AudioHealth audioHealth();

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

// The worst block of the last whole second as a percent of the block budget, 0 to 999. It has its own
// one-second window, so audioTakeLoad() does not disturb it. Reads 0 for the first second after boot.
int audioWorstLoadPercent();

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

// The energy of the input since the previous call (or since boot), for the OLED's voice screen. It
// is measured on the samples that enter the chain. Resets the window; call from the main loop.
level::InputEnergy audioTakeInputEnergy();

// The same for the output, measured on the left sample as written to the DAC, after the output
// gain and the soft ceiling. Resets its own window; call from the main loop.
level::OutputEnergy audioTakeOutputEnergy();

// The Input gate's detector keeps its highest level in two windows, so a status request and GATE
// LEARN each read the peak since their own last read.
enum class GateWindow : uint8_t { Status, Learn };

// The highest detector level (linear, full scale 1) since the previous call with the same window,
// or since boot. It reads 0 while no block has run the Input gate. Resets the window; call from the
// main loop.
float audioTakeGatePeak(GateWindow window);

AudioStats audioStats();

#if CUBEVOX_BENCH
// Clears late, overruns and maxCycles so a measurement can exclude the first blocks.
void audioResetPeaks();

// Stops the RX DMA so no more blocks arrive, to prove the stall watch. The TX ring goes silent
// within a ring, because no block refills it.
void audioStopForBench();

// Loops the clip compiled in by CUBEVOX_BENCH_CLIP in place of the SAI input. Returns the
// new state; false when no clip is compiled in.
bool audioBenchClip(bool on);

// True while the clip replaces the SAI input.
bool audioBenchClipOn();
#endif
