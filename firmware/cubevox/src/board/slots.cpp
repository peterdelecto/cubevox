#include "board/slots.h"

#include <array>

#include "board/pins.h"

// POT01..POT16 are the silkscreen numbers (hardware/PINMAP.md). Each constant is the 0-based mux
// input the pot is wired to: POT01-08 and POT16 sit on channel nn - 1, POT09-15 follow the board's
// west-row order.
//
// The board puts two pots in each panel column, one over the other. Column 1 holds POT03 over
// POT15, column 2 POT04 over POT16, and columns 3 to 7 hold POT05 over POT06 up to POT13 over
// POT14. Column 8 holds POT01 over POT02. Slot N takes the toggle and the pots of column N, and
// knob A is the upper pot.
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
constexpr uint8_t kPot15 = 12;
constexpr uint8_t kPot16 = 15;

const std::array<Slot, kSlotCount> kSlots = {{
    {pins::kToggle1, kPot03, kPot15},
    {pins::kToggle2, kPot04, kPot16},
    {pins::kToggle3, kPot05, kPot06},
    {pins::kToggle4, kPot07, kPot08},
    {pins::kToggle5, kPot09, kPot10},
    {pins::kToggle6, kPot11, kPot12},
    {pins::kToggle7, kPot13, kPot14},
    {pins::kToggle8, kPot01, kPot02},
}};

#if CUBEVOX_BENCH
int potNumber(int muxChannel) {
  static constexpr std::array<uint8_t, kPotCount> kChannelOfPot = {
      kPot01, kPot02, kPot03, kPot04, kPot05, kPot06, kPot07, kPot08,
      kPot09, kPot10, kPot11, kPot12, kPot13, kPot14, kPot15, kPot16};
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
