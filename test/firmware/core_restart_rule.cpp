// Checks the warm and cold start rule on the host. board/fault.cpp and board/power_sense.cpp only
// feed it the reset flags and the rail.

#include "support.h"

#include <cstdint>

#include "boot_decide.h"
#include "core/audio_fault_cause.h"
#include "core/mid_song_mark.h"
#include "core/restart_rule.h"

namespace {

constexpr uint32_t kPin = 1u << 22;  // PINRSTF

void checkMasks() {
  check(kResetFlagBor == CV_RSR_BORRSTF && kResetFlagPor == CV_RSR_PORRSTF && kResetFlagSoftware == CV_RSR_SFTRSTF &&
            kResetFlagWatchdog == CV_RSR_IWDG1RSTF && kResetFlagWindowWatchdog == CV_RSR_WWDG1RSTF,
        "restart rule: the masks are the bootloader's");
}

void checkWarm() {
  check(isWarmStart(kResetFlagSoftware, true, false), "restart rule: a software reset with a good rail is warm");
  check(isWarmStart(kResetFlagWatchdog, true, false), "restart rule: an IWDG reset with a good rail is warm");
  check(isWarmStart(kResetFlagWindowWatchdog, true, false), "restart rule: a WWDG reset with a good rail is warm");
  check(isWarmStart(kResetFlagSoftware | kResetFlagWatchdog, true, false),
        "restart rule: two box-caused flags are warm");
}

void checkCold() {
  check(!isWarmStart(kResetFlagPor, true, false), "restart rule: power-on is cold");
  check(!isWarmStart(kResetFlagBor, true, false), "restart rule: brown-out is cold");
  check(!isWarmStart(kResetFlagPor | kResetFlagBor | kPin, true, false), "restart rule: a power-up's flags are cold");
  check(!isWarmStart(0u, true, false), "restart rule: no flags is cold");
  check(!isWarmStart(kPin, true, false), "restart rule: a pin reset alone is cold");
  check(!isWarmStart(kResetFlagSoftware | kResetFlagPor, true, false),
        "restart rule: a power-loss flag beside a software one is cold");
  check(!isWarmStart(kResetFlagWatchdog | kResetFlagBor, true, false),
        "restart rule: brown-out beside a watchdog is cold");
}

void checkPinDecidesNothing() {
  check(isWarmStart(kResetFlagSoftware | kPin, true, false), "restart rule: PINRSTF beside a software reset stays warm");
  check(isWarmStart(kResetFlagWatchdog | kPin, true, false), "restart rule: PINRSTF beside a watchdog reset stays warm");
}

void checkRail() {
  check(!isWarmStart(kResetFlagSoftware, false, false), "restart rule: no power sense reading is cold");
  check(!isWarmStart(kResetFlagSoftware, true, true), "restart rule: a tripped rail is cold");
  check(!isWarmStart(kResetFlagSoftware, false, true), "restart rule: no reading and a trip is cold");
}

constexpr uint32_t kCrc = 0x3CD29860u;

void checkMidSongMark() {
  const MidSongMark mark = makeMidSongMark(kCrc);
  constexpr uint32_t v = kMidSongFieldsVersion;
  check(midSongMarkNames(v, mark, kCrc), "mark: a whole mark names its image");
  check(!midSongMarkNames(v, mark, kCrc + 1), "mark: another image's CRC does not count");
  check(!midSongMarkNames(v, MidSongMark{}, kCrc), "mark: a zeroed area is no mark");
  check(!midSongMarkNames(0u, mark, kCrc) && !midSongMarkNames(v + 1, mark, kCrc),
        "mark: only the fields version that wrote it counts");
  check(!midSongMarkNames(v, mark, 0u) && !midSongMarkNames(v, makeMidSongMark(0u), 0u),
        "mark: a running CRC of 0 is named by no mark");
  MidSongMark badMagic = mark;
  badMagic.magic ^= 1u;
  MidSongMark badCrc = mark;
  badCrc.imageCrc ^= 1u;
  MidSongMark badCheck = mark;
  badCheck.check ^= 1u;
  check(!midSongMarkNames(v, badMagic, kCrc) && !midSongMarkNames(v, badCrc, kCrc ^ 1u) &&
            !midSongMarkNames(v, badCheck, kCrc),
        "mark: a bit off in the magic, the CRC or the check word voids it");
  check(sizeof(MidSongMark) <= sizeof(CvBootBlock::appReserved), "mark: it fits the application area");
}

void checkMarkClearsFailedStarts() {
  const MidSongMark mark = makeMidSongMark(kCrc);
  constexpr uint32_t v = kMidSongFieldsVersion;
  check(clearsFailedStarts(kResetFlagSoftware, v, mark, kCrc), "mark: a software reset with a whole mark clears");
  check(clearsFailedStarts(kResetFlagWatchdog | kPin, v, mark, kCrc), "mark: a watchdog reset clears, PINRSTF or not");
  check(clearsFailedStarts(kResetFlagWindowWatchdog, v, mark, kCrc), "mark: a window watchdog reset clears");
  check(!clearsFailedStarts(kResetFlagPor, v, mark, kCrc), "mark: a power-on never clears");
  check(!clearsFailedStarts(kResetFlagSoftware | kResetFlagBor, v, mark, kCrc), "mark: brown-out beside a software reset never clears");
  check(!clearsFailedStarts(kPin, v, mark, kCrc) && !clearsFailedStarts(0u, v, mark, kCrc),
        "mark: a pin reset alone, or no flags, never clears");
  check(!clearsFailedStarts(kResetFlagSoftware, v, mark, kCrc + 1), "mark: after an update the old mark does not clear");
  check(!clearsFailedStarts(kResetFlagSoftware, v, MidSongMark{}, kCrc), "mark: no mark, no clear");

  CvBootBlock block;
  block.bootFails = 2;
  block.bootFailsCheck = ~2u;
  if (clearsFailedStarts(kResetFlagSoftware, v, mark, kCrc)) cvBootFailsSet(&block, 0u);
  check(cvBootFailsGet(&block) == 0u && cvBootFailsValid(&block), "mark: the counter reads 0 with its check word right");
}

void checkRestartCode() {
  using C = AudioFaultCause;
  check(restartCode(C::Stalled, false, kResetFlagSoftware) == C::Stalled, "code: a sealed audio cause wins");
  check(restartCode(C::StartFailed, true, kResetFlagSoftware) == C::StartFailed, "code: A4 wins over a fault record");
  check(restartCode(C::None, true, kResetFlagSoftware) == C::Crashed, "code: a crash with a fault record is C1");
  check(restartCode(C::None, false, kResetFlagWatchdog) == C::Hung, "code: a watchdog reset is W1");
  check(restartCode(C::None, false, kResetFlagWindowWatchdog | kPin) == C::Hung, "code: a window watchdog reset is W1");
  check(restartCode(C::None, false, kResetFlagSoftware) == C::None, "code: a plain software reset shows no code");
  check(restartCode(C::None, true, kResetFlagPor) == C::None && restartCode(C::None, false, kResetFlagWatchdog | kResetFlagBor) == C::None,
        "code: a power-up shows no C1 or W1");
  check(restartCode(C::None, false, 0u) == C::None, "code: no flags shows no code");
}

}  // namespace

void checkRestartRule() {
  checkMidSongMark();
  checkMarkClearsFailedStarts();
  checkRestartCode();
  checkMasks();
  checkWarm();
  checkCold();
  checkPinDecidesNothing();
  checkRail();
}
