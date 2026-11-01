// Checks the arithmetic behind the DEBUG load figure and the user LED: the worst and mean block of
// each whole second, their percent of the block budget, and the LED's lit share of each period. The
// block interrupt feeds the window and the main loop reads it.

#include "support.h"

#include <cstdint>

#include "core/load_led.h"
#include "core/load_window.h"

namespace {

// One second of 64-frame blocks at 48 kHz, and the budget of one block at 480 MHz.
constexpr uint32_t kSpan = 750;
constexpr uint32_t kBudget = 640000;

// The window reads 0 until a whole second has passed, then holds that second's worst block for the
// next second, whatever the blocks in between cost.
void checkWindowRolls() {
  LoadWindow w;
  for (uint32_t i = 0; i < kSpan - 1; ++i) w.add(1000 + i, kSpan);
  check(w.worst() == 0, "load: the figure reads 0 until the first second ends");
  w.add(5, kSpan);
  check(w.worst() == 1000 + kSpan - 2, "load: the first second's worst block shows when the second ends");

  for (uint32_t i = 0; i < kSpan - 1; ++i) w.add(900000, kSpan);
  check(w.worst() == 1000 + kSpan - 2, "load: a spike in the next second waits for that second to end");
  w.add(10, kSpan);
  check(w.worst() == 900000, "load: and shows when it does");

  for (uint32_t i = 0; i < kSpan; ++i) w.add(100, kSpan);
  check(w.worst() == 100, "load: a quiet second drops the spike, so the figure is the worst of the last second only");
}

// A spike in the middle of a second counts, and so does one in the second block.
void checkWindowSpikes() {
  LoadWindow mid;
  for (uint32_t i = 0; i < kSpan; ++i) mid.add(i == 300 ? 640000 : 10, kSpan);
  check(mid.worst() == 640000, "load: a spike mid-second is the second's worst");

  LoadWindow last;
  for (uint32_t i = 0; i < kSpan; ++i) last.add(i == kSpan - 1 ? 123456 : 10, kSpan);
  check(last.worst() == 123456, "load: a spike in the last block of a second counts in that second");

  LoadWindow next;
  for (uint32_t i = 0; i < kSpan + 1; ++i) next.add(i == kSpan ? 999999 : 10, kSpan);
  check(next.worst() == 10, "load: the first block of the next second is not in the finished one");
}

void checkPercent() {
  check(loadPercent(0, kBudget) == 0, "load: no cycles is 0%");
  check(loadPercent(44800, kBudget) == 7, "load: 44800 of 640000 cycles is 7%");
  check(loadPercent(268800, kBudget) == 42, "load: 268800 of 640000 cycles is 42%");
  check(loadPercent(kBudget, kBudget) == 100, "load: a block that uses its whole budget is 100%");
  check(loadPercent(3199, kBudget) == 0 && loadPercent(3200, kBudget) == 1, "load: the percent rounds to the nearest, half up");
  check(loadPercent(2 * kBudget, kBudget) == 200, "load: a block twice its budget is 200%");
  check(loadPercent(10 * kBudget, kBudget) == kMaxLoadPercent && loadPercent(0xFFFFFFFFu, kBudget) == kMaxLoadPercent,
        "load: the figure stops at 999%, with no overflow on the largest count");
  check(kMaxLoadPercent == 999, "load: the largest figure is 999%");
  check(loadPercent(1000, 0) == 0, "load: a zero budget reads 0%");
}

// A window of known block times gives the worst and the mean, both 0 until the first window closes.
void checkWindowMean() {
  LoadWindow w;
  for (uint32_t i = 0; i < kSpan - 1; ++i) w.add(i % 2 == 0 ? 100000 : 300000, kSpan);
  check(w.mean() == 0 && w.worst() == 0, "load: the mean and the worst read 0 until the first second ends");
  w.add(200000, kSpan);
  // 375 blocks of 100000, 374 of 300000 and the closing 200000, over 750 blocks.
  const uint64_t sum = 375ull * 100000 + 374ull * 300000 + 200000;
  check(w.mean() == sum / kSpan, "load: the mean is the second's total cycles over its blocks");
  check(w.worst() == 300000, "load: the worst stays the second's slowest block beside the mean");

  for (uint32_t i = 0; i < kSpan; ++i) w.add(64000, kSpan);
  check(w.mean() == 64000 && w.worst() == 64000, "load: a steady second reads the same mean and worst");

  for (uint32_t i = 0; i < kSpan; ++i) w.add(i == 0 ? 5 * kBudget : 0, kSpan);
  check(w.mean() == 5 * kBudget / kSpan, "load: a block over its budget counts in full, with no clipping");
  check(loadPercent(w.mean(), kBudget) == 1, "load: one overrun block in a second moves the mean percent by a hair");

  LoadWindow big;
  for (uint32_t i = 0; i < kSpan; ++i) big.add(0xFFFFFFFFu, kSpan);
  check(big.mean() == 0xFFFFFFFFu, "load: the running sum keeps past 32 bits, so the mean of the largest blocks is exact");

  LoadWindow part;
  for (uint32_t i = 0; i < kSpan; ++i) part.add(kBudget, kSpan);
  check(loadPercent(part.mean(), kBudget) == 100, "load: blocks that use their whole budget average 100%");
}

// Counts the milliseconds of one period, from `startMs`, that the LED is lit.
uint32_t litMs(uint32_t startMs, int percent, bool running) {
  uint32_t lit = 0;
  for (uint32_t ms = startMs; ms < startMs + kLoadLedPeriodMs; ++ms) {
    if (loadLedLit(ms, percent, running)) ++lit;
  }
  return lit;
}

void checkLedShare() {
  check(kLoadLedPeriodMs == 500, "led: the period is 500 ms");
  check(litMs(0, 0, true) == 0, "led: 0% is never lit");
  check(litMs(0, 10, true) == 50, "led: 10% is a 50 ms blip");
  check(litMs(0, 50, true) == 250, "led: 50% is half on");
  check(litMs(0, 90, true) == 450, "led: 90% is on for 450 ms with a 50 ms dark gap");
  check(litMs(0, 100, true) == 500, "led: 100% is solid");
  check(litMs(0, 150, true) == 500, "led: 150% is held to solid");
  check(litMs(0, -5, true) == 0, "led: a negative percent is held to dark");
  check(litMs(0, 10, false) == 0 && litMs(0, 50, false) == 0 && litMs(0, 100, false) == 0,
        "led: audio off is never lit at any load");
}

// The lit stretch opens with each period and closes at the exact millisecond.
void checkLedEdges() {
  check(loadLedLit(0, 10, true) && loadLedLit(49, 10, true), "led: 10% is lit at ms 0 and ms 49");
  check(!loadLedLit(50, 10, true) && !loadLedLit(499, 10, true), "led: 10% is dark from ms 50 to the period's end");
  check(loadLedLit(500, 10, true) && loadLedLit(549, 10, true) && !loadLedLit(550, 10, true),
        "led: the next period repeats it");
  check(loadLedLit(249, 50, true) && !loadLedLit(250, 50, true), "led: 50% turns off at ms 250");
  check(loadLedLit(449, 90, true) && !loadLedLit(450, 90, true) && !loadLedLit(499, 90, true),
        "led: 90% turns off at ms 450 and stays off to the period's end");
  check(loadLedLit(499, 100, true) && loadLedLit(500, 100, true), "led: 100% is lit on both sides of the period edge");
  check(litMs(123456, 10, true) == 50 && litMs(123456, 50, true) == 250,
        "led: a period that starts mid-count holds the same share");
}

}  // namespace

void checkLoadWindow() {
  checkWindowRolls();
  checkWindowSpikes();
  checkWindowMean();
  checkPercent();
  checkLedShare();
  checkLedEdges();
}
