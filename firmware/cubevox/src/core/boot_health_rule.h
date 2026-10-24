// The rules behind the boot health report (spec 2026-10-24, Boot sequence 6). Pure: board/boot_health.cpp
// supplies the clock, the audio block counter and the boot block.

#pragma once

#include <stdint.h>

// True once `windowMs` have passed since setup() ended and the audio interrupt advanced blocks in
// that time. The window comes from the caller because CV_HEALTHY_MS lives in board/boot_block.h,
// which core does not include. The unsigned subtraction stays right across a millis() wrap.
constexpr bool bootHealthy(uint32_t setupDoneMs, uint32_t nowMs, uint32_t blocksAtSetup, uint32_t blocksNow,
                           uint32_t windowMs) {
  return nowMs - setupDoneMs >= windowMs && blocksNow != blocksAtSetup;
}

// The bootloader counts a start before it jumps, so the counter it leaves includes the start that
// is running. This is the number of earlier starts that never reported healthy. A counter of 0
// means the bootloader did not write it, and that also reads as no earlier failure.
constexpr uint32_t failedStartsBefore(uint32_t counterAtStart) {
  return counterAtStart > 0 ? counterAtStart - 1 : 0;
}
