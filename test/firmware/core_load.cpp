// Checks the arithmetic behind the DEBUG load figure: the worst block of each whole second and its
// percent of the block budget. The block interrupt feeds the window and the main loop reads it.

#include "support.h"

#include <cstdint>

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

}  // namespace

void checkLoadWindow() {
  checkWindowRolls();
  checkWindowSpikes();
  checkPercent();
}
