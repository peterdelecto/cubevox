#include "board/slots.h"

#include "board/pins.h"

// POT15 is KEY and POT16 is SEMITONES (hardware/PINMAP.md). The other fourteen pots fill
// POT01..POT14 left to right, so KEY and SEMITONES sit out of left-to-right order.
// Channel numbers are 0-based mux inputs. POT01-08 and POT16 are channel nn - 1; POT09-15 are
// re-mapped to match the board's west-row order (hardware/PINMAP.md, 2026-10-08).
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

const Slot kSlots[kSlotCount] = {
    {pins::kToggle1, kPot01, kPot02},
    {pins::kToggle2, kPot15Key, kPot03},
    {pins::kToggle3, kPot16Semitones, kPot04},
    {pins::kToggle4, kPot05, kPot06},
    {pins::kToggle5, kPot07, kPot08},
    {pins::kToggle6, kPot09, kPot10},
    {pins::kToggle7, kPot11, kPot12},
    {pins::kToggle8, kPot13, kPot14},
};
