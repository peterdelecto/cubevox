// Checks the arithmetic behind the DEBUG load figure and the user LED: the worst and mean block of
// each whole second, their percent of the block budget, and the LED's level: its curve from dark
// to full, the flash after a late block and the clock that times it. The block interrupt feeds the
// window and the main loop reads it.

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

constexpr uint32_t kLimit = kLoadLedLimitPercent;

// The level when nothing has gone wrong lately.
float calm(int percent, bool live = true) {
  return loadLedLevel(percent, kLoadLedLimitPercent, live, kLoadLedNoTrouble);
}

bool near(float a, float b) { return a - b < 1e-5f && b - a < 1e-5f; }

void checkLedCurve() {
  check(kLoadLedLimitPercent == 75, "led: full at the 75 % average line a layout is gated against");
  check(near(calm(0), kLoadLedIdle) && kLoadLedIdle > 0.0f, "led: zero load glows at the idle level, not dark");
  check(near(calm(static_cast<int>(kLimit)), 1.0f), "led: the layout limit is full");
  check(near(calm(500), 1.0f) && near(calm(kMaxLoadPercent), 1.0f), "led: a load past the limit is held to full");
  check(near(calm(-5), kLoadLedIdle), "led: a negative load is held to the idle level");

  float last = calm(0);
  bool rises = true;
  for (int p = 1; p <= static_cast<int>(kLimit); ++p) {
    const float now = calm(p);
    if (now <= last) rises = false;
    last = now;
  }
  check(rises, "led: every percent of load from 0 to the limit is brighter than the one before");

  const float half = calm(static_cast<int>(kLimit) / 2);
  check(half > 0.2f && half < 0.3f, "led: half the limit is about a quarter of full, the square of the share");
  check(near(loadLedLevel(0, 0, true, kLoadLedNoTrouble), 1.0f), "led: a zero limit reads full, not a divide by zero");
}

// Audio stopped, stalled or latched keeps the LED dark whatever the load or a recent late block.
void checkLedDark() {
  check(near(calm(0, false), 0.0f) && near(calm(50, false), 0.0f) && near(calm(999, false), 0.0f),
        "led: audio not live is dark at any load");
  check(near(loadLedLevel(10, kLoadLedLimitPercent, false, 0), 0.0f),
        "led: a late block does not light the LED while audio is not live");
}

void checkLedFlash() {
  check(near(loadLedLevel(0, kLoadLedLimitPercent, true, 0), 1.0f), "led: a late block flashes full at once, at idle load");
  check(near(loadLedLevel(0, kLoadLedLimitPercent, true, kLoadLedFlashMs - 1), 1.0f),
        "led: the flash holds through its 100th millisecond");
  check(near(loadLedLevel(0, kLoadLedLimitPercent, true, kLoadLedFlashMs), kLoadLedIdle),
        "led: and ends at 100 ms, back on the load's level");
  check(near(loadLedLevel(30, kLoadLedLimitPercent, true, 5000), calm(30)), "led: a late block long ago changes nothing");
}

void checkTroubleClock() {
  BlockTroubleClock c;
  check(c.sinceMs(1000) == kLoadLedNoTrouble, "trouble: none before the first count");
  c.note(7, 1000);
  check(c.sinceMs(1000) == kLoadLedNoTrouble, "trouble: the first count only sets the baseline");
  c.note(7, 1050);
  check(c.sinceMs(1050) == kLoadLedNoTrouble, "trouble: an unchanged count is none");
  c.note(8, 1100);
  check(c.sinceMs(1100) == 0 && c.sinceMs(1150) == 50, "trouble: a count that rose is timed from the pass that saw it");
  c.note(8, 1200);
  check(c.sinceMs(1200) == 100, "trouble: it keeps counting while the count holds");
  c.note(5, 1300);
  check(c.sinceMs(1300) == 0, "trouble: a count that fell, as a peaks reset does, also restarts it");
  BlockTroubleClock w;
  w.note(0, 0xFFFFFF00u);
  w.note(1, 0xFFFFFFF0u);
  check(w.sinceMs(0x10u) == 0x20u, "trouble: the time survives the millisecond counter wrapping");
}

}  // namespace

void checkLoadWindow() {
  checkWindowRolls();
  checkWindowSpikes();
  checkWindowMean();
  checkPercent();
  checkLedCurve();
  checkLedDark();
  checkLedFlash();
  checkTroubleClock();
}
