// Checks the health rules on the host: when a start counts as healthy, how many earlier starts
// failed, and when the audio stream counts as stalled. board/boot_health.cpp and board/fault.cpp
// only feed these rules the clock, the audio state and the boot block.

#include "support.h"

#include <cstdint>

#include "board/boot_block.h"
#include "core/audio_stall.h"
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

// setup() spends 300 ms in the panel's reset delays before the stream starts, and the first look
// comes before the first block.
void checkStallArmsAtStreamStart() {
  AudioStallWatch w;
  check(!w.update(300, true, 0, 0), "stall: the first look arms the watch and is no stall");
  check(!w.update(301, true, 0, 0) && !w.update(300 + kAudioStallMs - 1, true, 0, 0),
        "stall: the time setup() spent before the stream started never counts");
  check(w.update(300 + kAudioStallMs, true, 0, 0) && w.stalled(), "stall: a stream that never advances stalls after the window");
  check(!w.update(300 + 2 * kAudioStallMs, true, 0, 0) && w.stalled(), "stall: it reports once and stays stalled");
}

void checkStallFollowsBlocks() {
  AudioStallWatch w;
  w.update(0, true, 0, 0);
  const uint32_t last = 10 * kAudioStallMs;  // one block per ms until then
  bool any = false;
  for (uint32_t t = 1; t <= last; ++t) any = w.update(t, true, 0, t) || any;
  check(!any && !w.stalled(), "stall: blocks that keep advancing never stall");
  check(!w.update(last + kAudioStallMs - 1, true, 0, last) && w.update(last + kAudioStallMs, true, 0, last),
        "stall: the window runs from the last block");
  check(w.blocks() == last, "stall: it keeps the count it stalled at");
}

void checkStallSkipsStoppedStream() {
  AudioStallWatch idle;
  check(!idle.update(5000, false, 0, 0) && !idle.update(9000, false, 0, 0), "stall: no stream, no stall");

  AudioStallWatch latched;
  latched.update(0, true, 0, 0);
  check(!latched.update(50, false, 0, 0) && !latched.update(50 + 10 * kAudioStallMs, false, 0, 0),
        "stall: a stream the audio module latched is not a stall");

  AudioStallWatch restarted;
  restarted.update(0, true, 0, 0);
  check(!restarted.update(80, true, 1, 0) && !restarted.update(80 + kAudioStallMs - 1, true, 1, 0),
        "stall: a restart arms the watch afresh, so its gap is not a stall");
  check(restarted.update(80 + kAudioStallMs, true, 1, 0), "stall: a restarted stream that stays still still stalls");
}

void checkStallClockWrap() {
  AudioStallWatch w;
  const uint32_t start = 0xFFFFFFF0u;
  w.update(start, true, 0, 7);
  check(!w.update(start + kAudioStallMs - 1, true, 0, 7) && w.update(start + kAudioStallMs, true, 0, 7),
        "stall: the window holds across a millisecond wrap");
}

}  // namespace

void checkBootHealth() {
  checkHealthyWindow();
  checkBlocksMustAdvance();
  checkClockWrap();
  checkWindowIsTheCallers();
  checkFailedStarts();
  checkStallArmsAtStreamStart();
  checkStallFollowsBlocks();
  checkStallSkipsStoppedStream();
  checkStallClockWrap();
}
