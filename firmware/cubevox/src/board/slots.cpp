#include "board/slots.h"

#include <array>

#include "board/pins.h"

// POT01..POT16 are the silkscreen numbers (hardware/PINMAP.md). Each constant is the 0-based mux
// input the pot is wired to: POT01-08 and POT16 sit on channel nn - 1, POT09-15 follow the board's
// west-row order. POT15 is KEY and POT16 is SEMITONES, so they sit out of left-to-right order in
// the slot table below.
constexpr uint8_t kPot01 = 0;
constexpr uint8_t kPot02 = 1;
constexpr uint8_t kPot03 = 2;
constexpr uint8_t kPot04 = 3;
constexpr uint8_t kPot05 = 4;
constexpr uint8_t kPot06 = 5;
constexpr uint8_t kPot07 = 6;
constexpr uint8_t kPot08 = 7;
constexpr uint8_t kPot09 = 13;
constexpr uint8_t kPot10 = 14;
constexpr uint8_t kPot11 = 9;
constexpr uint8_t kPot12 = 11;
constexpr uint8_t kPot13 = 8;
constexpr uint8_t kPot14 = 10;
constexpr uint8_t kPot15Key = 12;
constexpr uint8_t kPot16Semitones = 15;

const std::array<Slot, kSlotCount> kSlots = {{
    {pins::kToggle1, kPot01, kPot02},
    {pins::kToggle2, kPot15Key, kPot03},
    {pins::kToggle3, kPot16Semitones, kPot04},
    {pins::kToggle4, kPot05, kPot06},
    {pins::kToggle5, kPot07, kPot08},
    {pins::kToggle6, kPot09, kPot10},
    {pins::kToggle7, kPot11, kPot12},
    {pins::kToggle8, kPot13, kPot14},
}};

#if CUBEVOX_BENCH
int potNumber(int muxChannel) {
  static constexpr std::array<uint8_t, kPotCount> kChannelOfPot = {
      kPot01, kPot02, kPot03, kPot04, kPot05, kPot06, kPot07, kPot08,
      kPot09, kPot10, kPot11, kPot12, kPot13, kPot14, kPot15Key, kPot16Semitones};
  for (size_t i = 0; i < kChannelOfPot.size(); ++i) {
    if (kChannelOfPot[i] == muxChannel) return static_cast<int>(i) + 1;
  }
  return 0;
}
#endif

PotOwner potOwner(int muxChannel) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (kSlots[s].muxChannelA == muxChannel) return {static_cast<int8_t>(s), false};
    if (kSlots[s].muxChannelB == muxChannel) return {static_cast<int8_t>(s), true};
  }
  return {-1, false};
}
