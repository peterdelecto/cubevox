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

// The default layout with the Crowd card in slot 2, Autotune's slot. Only the CUBEVOX_CROWDTEST image uses it.
Layout crowdTestLayout();

// The slot that holds a reverb card (Spring or Chasm). A valid layout always holds one; -1 only
// for a layout that failed layoutValid.
int reverbSlot(const Layout& layout);

// The slot that holds Unison, Crowd or Mob 2, the first one when several are placed; -1 when none is.
int unisonCrowdSlot(const Layout& layout);

// The slot that holds Octave, Formant or the Formant follower, the first one when several are placed;
// -1 when none is. The menu's slot 3 row cycles the card in it.
int octaveFormantSlot(const Layout& layout);

// The slot that holds Distortion or Growl, the first one when both are placed; -1 when neither is.
int distortionGrowlSlot(const Layout& layout);

// The slot that holds Gate, Lo-fi or Chatter, the first one when several are placed; -1 when none is.
// The menu's slot 7 row cycles the card in it.
int slot7CardSlot(const Layout& layout);

// The name to show for a stored layout name: the name itself, or "Default" when it is empty.
const char* layoutDisplayName(const char* stored);

enum class LayoutError : uint8_t { None, BadCard, SameCardTwice, TwoReverbs, NoReverb, OverAvg, OverWorst, TwoMobs };

// Outcome of layoutValid. The fields beside `error` say which slot, card or percentage broke the rule.
struct LayoutCheck {
  LayoutError error = LayoutError::None;
  int slot = 0;      // BadCard: zero-based slot holding the out-of-range id
  Card card = Card::Empty;  // SameCardTwice: the repeated card; TwoMobs: the second of the two
  int percent = 0;   // OverAvg, OverWorst: the layout's sum, including the fixed term

  bool ok() const { return error == LayoutError::None; }
};

// The cost sums layoutValid holds against the budget lines, in percent of a block: the fixed term, each
// placed card's row, the pitch tracker's row once when Autotune, Crowd, Mob 2, Formant, the Formant
// follower or Growl is placed, and the pitch stage's row once when Octave, Formant or the Formant
// follower is placed without Autotune. Every card id must be in range.
struct LayoutCost {
  int avg = 0;
  int worst = 0;
};
LayoutCost layoutCost(const Layout& layout);

// Placement rules of the registry spec: ids in range, no card twice (Empty excepted, and Formant and the
// Formant follower counting as one card), exactly one reverb card, at most one of Mob and Mob 2 (they
// share the Crowd engine), and the layoutCost sums under the average and worst-block lines.
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
