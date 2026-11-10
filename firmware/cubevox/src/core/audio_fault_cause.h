// The causes that end a run on an audio fault, pure. board/fault.cpp seals the cause in .cv_noinit
// before the reset and takes it at the next boot, so the splash and DIAG.TXT name the same error.

#pragma once

#include <stdint.h>

// The values are the codes A1 to A3.
enum class AudioFaultCause : uint8_t {
  None = 0,
  RepeatedFaults = 1,  // A1: a fault past the restart budget
  RestartFailed = 2,   // A2: a stream restart failed
  Stalled = 3,         // A3: no block finished for kAudioStallMs while the stream ran
};

// A sealed word holds this in its upper 24 bits and the cause in its low byte. A power-on leaves the
// word random, so about three power-ons in 2^32 read a cause.
constexpr uint32_t kAudioFaultSeal = 0x5A0D1000u;

constexpr uint32_t sealAudioFault(AudioFaultCause cause) { return kAudioFaultSeal | static_cast<uint8_t>(cause); }

// The cause `stored` holds, or None when it is not a sealed cause. Clears it either way, so one fault
// gives one boot with the cause.
inline AudioFaultCause takeAudioFaultSeal(uint32_t& stored) {
  const uint32_t cause = stored & 0xFFu;
  const bool sealed = (stored & ~0xFFu) == kAudioFaultSeal && cause >= 1 && cause <= 3;
  stored = 0;
  return sealed ? static_cast<AudioFaultCause>(cause) : AudioFaultCause::None;
}

// The code, "A1" to "A3", or "" for None.
constexpr const char* audioFaultCode(AudioFaultCause cause) {
  switch (cause) {
    case AudioFaultCause::RepeatedFaults: return "A1";
    case AudioFaultCause::RestartFailed: return "A2";
    case AudioFaultCause::Stalled: return "A3";
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
    case AudioFaultCause::None: break;
  }
  return "";
}
