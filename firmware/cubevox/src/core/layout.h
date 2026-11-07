// Which card sits in each of the kSlotCount slots. The chain order is the slot order.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "core/registry.h"
#include "core/slots.h"

// Bytes of a layout name including the NUL; a name holds 1 to kLayoutNameBytes - 1 printable ASCII characters.
constexpr size_t kLayoutNameBytes = 32;

struct Layout {
  std::array<Card, kSlotCount> card;
};

inline bool operator==(const Layout& a, const Layout& b) { return a.card == b.card; }
inline bool operator!=(const Layout& a, const Layout& b) { return !(a == b); }

// Slot 1 to kSlotCount as shipped.
Layout defaultLayout();

// The slot that holds a reverb card (Spring or Chasm). A valid layout always holds one; -1 only
// for a layout that failed layoutValid.
int reverbSlot(const Layout& layout);

// The slot that holds Unison or Mob, the first one when both are placed; -1 when neither is.
int unisonCrowdSlot(const Layout& layout);

// The slot that holds Octave, Formant or the Formant follower, the first one when several are placed;
// -1 when none is. The menu's slot 3 row cycles the card in it.
int octaveFormantSlot(const Layout& layout);

// The slot that holds Distortion or Deepthroat, the first one when several are placed; -1 when none is.
int distortionGrowlSlot(const Layout& layout);

// The slot that holds Gate, Lo-fi or Chatter, the first one when several are placed; -1 when none is.
// The menu's slot 7 row cycles the card in it.
int slot7CardSlot(const Layout& layout);

// The five slots whose card the menu swaps (slots 3, 4, 6, 7 and 8, in slot order).
enum class SwapSlot : uint8_t { OctaveFormant, UnisonMob, DistortionDeepthroat, Slot7, Reverb, Count };

// The cards a swappable slot can hold, in the menu's cycle order, and the menu's word for each. The
// first card is the default: a layout holding none of them reads and cycles as the first.
struct SwapCards {
  size_t count;
  std::array<Card, 3> card;
  std::array<const char*, 3> word;
};
const SwapCards& swapCards(SwapSlot which);

// The slot that holds one of `which`'s cards, the first one when several are placed; -1 when none is.
int swapSlotOf(const Layout& layout, SwapSlot which);

// The card after `current` in `which`'s cycle, wrapping to the first. A card outside the set gives the first.
Card nextSwapCard(SwapSlot which, Card current);

// The menu's word for the card of `which` that `layout` holds, the first card's word when it holds none.
const char* swapWord(const Layout& layout, SwapSlot which);

// The name to show for a stored layout name: the name itself, or "Default" when it is empty.
const char* layoutDisplayName(const char* stored);

enum class LayoutError : uint8_t { None, BadCard, SameCardTwice, TwoReverbs, NoReverb, OverAvg, OverWorst };

// Outcome of layoutValid. The fields beside `error` say which slot, card or percentage broke the rule.
struct LayoutCheck {
  LayoutError error = LayoutError::None;
  int slot = 0;      // BadCard: zero-based slot holding the out-of-range id
  Card card = Card::Empty;  // SameCardTwice: the repeated card
  int percent = 0;   // OverAvg, OverWorst: the layout's sum, including the fixed term

  bool ok() const { return error == LayoutError::None; }
};

// The cost sums layoutValid holds against the budget lines, in percent of a block: the fixed term, each
// placed card's row, and one tracker row for Autotune plus one for each Formant or Apraxia card not
// directly behind Autotune and each Mob or Deepthroat card with no Autotune ahead. Every card id must
// be in range.
struct LayoutCost {
  int avg = 0;
  int worst = 0;
};
LayoutCost layoutCost(const Layout& layout);

// Placement rules of the registry spec: ids in range, no card twice (Empty excepted, and Formant and the
// Formant follower counting as one card), exactly one reverb card, and the layoutCost sums under the average and worst-block lines.
LayoutCheck layoutValid(const Layout& layout);

// Short readable reason for a failed check, for example "two reverb cards". Empty string when ok.
void layoutReason(const LayoutCheck& check, char* buf, size_t size);

// What recoverLayout did to a layout that came from storage.
struct Recovery {
  bool replaced = false;  // the layout failed layoutValid and now holds the default layout
  LayoutCheck why;        // the failed check, ok when nothing was replaced
};

// Keeps `layout` when layoutValid accepts it, otherwise replaces the whole layout with the default.
Recovery recoverLayout(Layout& layout);
