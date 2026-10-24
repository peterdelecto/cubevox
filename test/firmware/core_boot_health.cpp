// Checks the boot health rules on the host: when a start counts as healthy and how many earlier
// starts failed. board/boot_health.cpp only feeds these rules the clock, the audio block counter
// and the boot block.

#include "support.h"

#include <cstdint>

#include "board/boot_block.h"
#include "core/boot_health_rule.h"

namespace {

constexpr uint32_t kWindow = CV_HEALTHY_MS;

void checkHealthyWindow() {
  check(CV_HEALTHY_MS == 5000u, "health: the window is 5 s");
  check(!bootHealthy(1000, 1000, 5, 6, kWindow), "health: not healthy when the window opens");
  check(!bootHealthy(1000, 1000 + kWindow - 1, 5, 9, kWindow), "health: not healthy 1 ms before the window ends");
  check(bootHealthy(1000, 1000 + kWindow, 5, 9, kWindow), "health: healthy when the window ends with blocks advanced");
  check(bootHealthy(1000, 1000 + 20 * kWindow, 5, 9, kWindow), "health: still healthy long after the window");
}

void checkBlocksMustAdvance() {
  check(!bootHealthy(1000, 1000 + kWindow, 5, 5, kWindow), "health: audio that never advanced is not healthy");
  check(!bootHealthy(1000, 1000 + 20 * kWindow, 5, 5, kWindow), "health: waiting longer does not make a silent box healthy");
  check(bootHealthy(1000, 1000 + kWindow, 5, 6, kWindow), "health: a single block of progress counts");
  check(bootHealthy(1000, 1000 + kWindow, 0xFFFFFFFFu, 3, kWindow), "health: the block counter may wrap");
}

void checkClockWrap() {
  const uint32_t setupDone = 0xFFFFFF00u;
  const uint32_t justBefore = setupDone + kWindow - 1;  // wraps past zero
  const uint32_t atEnd = setupDone + kWindow;
  check(!bootHealthy(setupDone, justBefore, 1, 2, kWindow), "health: 1 ms short across a millisecond wrap");
  check(bootHealthy(setupDone, atEnd, 1, 2, kWindow), "health: the window ends across a millisecond wrap");
}

void checkWindowIsTheCallers() {
  check(bootHealthy(0, 100, 1, 2, 100) && !bootHealthy(0, 99, 1, 2, 100), "health: the window length comes from the caller");
  check(bootHealthy(0, 0, 1, 2, 0), "health: a zero window needs only the blocks");
}

void checkFailedStarts() {
  check(failedStartsBefore(1) == 0, "fails: the start itself is not a failure");
  check(failedStartsBefore(2) == 1, "fails: a second start without a healthy report is one failure");
  check(failedStartsBefore(CV_BOOT_FAIL_LIMIT) == CV_BOOT_FAIL_LIMIT - 1,
        "fails: the last start the bootloader allows reads one short of the limit");
  check(failedStartsBefore(0) == 0, "fails: a counter the bootloader did not write reads as no failure");
  check(failedStartsBefore(0xFFFFFFFFu) == 0xFFFFFFFEu, "fails: the largest counter does not wrap");
}

}  // namespace

void checkBootHealth() {
  checkHealthyWindow();
  checkBlocksMustAdvance();
  checkClockWrap();
  checkWindowIsTheCallers();
  checkFailedStarts();
}
