// Audio path: SAI1 master TX to the PCM5102A and slave RX from the PCM1808 (both I2S, 32-bit
// slots, 48 kHz, MCLK 12.288 MHz on PE2). Each 64-frame block runs the engine chain in slot
// order from a low-priority interrupt that the RX DMA half-transfer pends.

#pragma once

#include <stdint.h>

#include "core/chain_params.h"

struct AudioStats {
  uint32_t blocks = 0;        // blocks processed
  uint32_t late = 0;          // blocks that took longer than the budget (the DMA played stale data)
  uint32_t overruns = 0;      // blocks that were not even started before the next one arrived
  uint32_t maxCycles = 0;     // worst block time in CPU cycles
  uint32_t avgCycles = 0;     // mean block time since the last peaks reset
  uint32_t budgetCycles = 0;  // CPU cycles available per block
  uint32_t slotAvg[kSlotCount + 1] = {};  // mean cycles per slot since the reset; last is the EQ
};

// Brings up PLL3, SAI1 and DMA, and starts the stream. Returns false on failure; see audioError().
bool audioInit();
const char* audioError();

// Hands a complete parameter set to the audio interrupt. Lock-free, call from the main loop.
void audioPublish(const ChainParams& params);

AudioStats audioStats();

// Clears late, overruns and maxCycles so a measurement can exclude the first blocks.
void audioResetPeaks();

// Stops the RX DMA so no more blocks arrive. Bench use only, to prove the stall watch.
void audioStopForBench();
