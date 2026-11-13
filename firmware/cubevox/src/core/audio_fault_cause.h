// The causes behind a restart code, pure. A1 to A4 are audio faults: board/fault.cpp seals the cause in
// .cv_noinit before the reset and takes it at the next boot, so the splash and DIAG.TXT name the same
// error. C1 and W1 are never sealed; restartCode() reads them from the fault record and the reset
// flags.

#pragma once

#include <stdint.h>

#include "core/restart_rule.h"

// The values are the codes A1 to A4, then C1 and W1.
enum class AudioFaultCause : uint8_t {
  None = 0,
  RepeatedFaults = 1,  // A1: a fault past the restart budget
  RestartFailed = 2,   // A2: a stream restart failed
  Stalled = 3,         // A3: no block finished for kAudioStallMs while the stream ran
  StartFailed = 4,     // A4: the audio did not start on a warm start
  Crashed = 5,         // C1: the processor faulted
  Hung = 6,            // W1: the watchdog ended a hang
};

// The causes board/fault.cpp seals across a reset.
constexpr uint8_t kLastSealedCause = static_cast<uint8_t>(AudioFaultCause::StartFailed);

// A sealed word holds this in its upper 24 bits and the cause in its low byte. A power-on leaves the
// word random, so about four power-ons in 2^32 read a cause.
constexpr uint32_t kAudioFaultSeal = 0x5A0D1000u;

constexpr uint32_t sealAudioFault(AudioFaultCause cause) { return kAudioFaultSeal | static_cast<uint8_t>(cause); }

// The cause `stored` holds, or None when it is not a sealed cause. Clears it either way, so one fault
// gives one boot with the cause.
inline AudioFaultCause takeAudioFaultSeal(uint32_t& stored) {
  const uint32_t cause = stored & 0xFFu;
  const bool sealed = (stored & ~0xFFu) == kAudioFaultSeal && cause >= 1 && cause <= kLastSealedCause;
  stored = 0;
  return sealed ? static_cast<AudioFaultCause>(cause) : AudioFaultCause::None;
}

// The code the splash and DIAG.TXT show for this start. A sealed audio cause wins. Otherwise a box-
// caused reset with a fault record is C1, and one the watchdog ended is W1. Anything else is None.
constexpr AudioFaultCause restartCode(AudioFaultCause sealed, bool faultRecordPresent, uint32_t resetFlags) {
  if (sealed != AudioFaultCause::None) return sealed;
  if (!isBoxCausedReset(resetFlags)) return AudioFaultCause::None;
  if (faultRecordPresent) return AudioFaultCause::Crashed;
  if ((resetFlags & (kResetFlagWatchdog | kResetFlagWindowWatchdog)) != 0u) return AudioFaultCause::Hung;
  return AudioFaultCause::None;
}

// The code, "A1" to "A4", "C1" or "W1", or "" for None.
constexpr const char* audioFaultCode(AudioFaultCause cause) {
  switch (cause) {
    case AudioFaultCause::RepeatedFaults: return "A1";
    case AudioFaultCause::RestartFailed: return "A2";
    case AudioFaultCause::Stalled: return "A3";
    case AudioFaultCause::StartFailed: return "A4";
    case AudioFaultCause::Crashed: return "C1";
    case AudioFaultCause::Hung: return "W1";
    case AudioFaultCause::None: break;
  }
  return "";
}

// The fault text of the cause's DIAG.TXT entry: the code, then the cause in words.
constexpr const char* audioFaultDiagText(AudioFaultCause cause) {
  switch (cause) {
    case AudioFaultCause::RepeatedFaults: return "A1 repeated stream faults";
    case AudioFaultCause::RestartFailed: return "A2 stream restart failed";
    case AudioFaultCause::Stalled: return "A3 audio stalled";
    case AudioFaultCause::StartFailed: return "A4 audio did not start";
    case AudioFaultCause::Crashed: return "C1 processor fault";
    case AudioFaultCause::Hung: return "W1 watchdog reset";
    case AudioFaultCause::None: break;
  }
  return "";
}
