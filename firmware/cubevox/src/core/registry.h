// Effect registry: the cards Adam can place in a slot, their knobs, groups and costs.
// Firmware is the only source of truth; the Mac app learns this table over `hello`.
// Spec: docs/specs/2026-10-08-effect-registry-design.md.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/chain_params.h"
#include "core/costs.h"
#include "core/slots.h"

// Table index. Ids are wire-stable once shipped; append only. Count ends the list.
enum class Card : uint8_t {
  Empty,
  InputGate,
  Autotune,
  Octave,
  Unison,
  Slapback,
  Distortion,
  Gate,
  ReverbSpring,
  ReverbChasm,
  Count,
};

constexpr int kCardCount = static_cast<int>(Card::Count);

// Bytes of a card id including the NUL. A card id read from the wire or flash is clipped to this,
// so the registry keeps every id shorter than it by at least two characters.
constexpr size_t kCardIdBytes = 16;

// Engines the audio block owns. Cards map onto them: Autotune and Octave share Pitch, and both
// reverb cards share Reverb. Count ends the list.
enum class Engine : uint8_t { InputGate, Pitch, Unison, Slapback, Distortion, Gate, Reverb, Count };

constexpr int kEngineCount = static_cast<int>(Engine::Count);

// The engine of a card that runs none (Empty).
constexpr Engine kNoEngine = Engine::Count;

enum class KnobKind : uint8_t { Continuous, Stepped };
enum class Group : uint8_t { None, Pitch, Reverb };

// Maps the knob's 0..1 position onto min..max. LogReversed runs geometrically from max at 0 to min at 1.
enum class Curve : uint8_t { Linear, Log, LogReversed };

struct KnobDesc {
  const char* name;
  KnobKind kind;
  uint8_t positions;  // stepped only; 0 for continuous
  float min;          // in `unit`; min < max always; bipolar knobs have min < 0 < max
  float max;
  const char* unit;   // "", "dB", "ms", "st", "%"
  Curve curve = Curve::Linear;
  uint8_t decimals = 0;                 // digits after the point on the OLED, 0 for whole numbers
  const char* const* labels = nullptr;  // names of a stepped knob's positions, shown in place of a number
};

struct CardDesc {
  Card card;          // its own table index
  const char* id;     // wire id, snake_case
  const char* name;   // OLED and app display name
  Engine engine;      // kNoEngine for Empty
  uint8_t knobCount;  // 0 for Empty
  KnobDesc knob[kKnobsPerCard];
  Group group;
  costs::Cost cost;   // percent of one audio block, mean and worst, and whether a board measured it; costs.h
};

// The descriptor of `c`; an out-of-range card reads as Empty.
const CardDesc& cardDesc(Card c);

// The engine that runs `c`, kNoEngine for Empty.
Engine engineOf(Card c);

// Looks a card up by its wire id. False when no card has that id.
bool cardFromId(const char* id, Card& out);

// The value a continuous knob holds at position 0..1, in the descriptor's unit. knobShown uses it for
// every continuous knob; applyKnob uses it only where the engine field is in that unit (dB, ms), and
// hands percent knobs the 0..1 position directly.
float knobValue(const KnobDesc& k, float position);

// 0 for a continuous knob or an out-of-range card or knob, otherwise the number of positions it snaps to.
int knobSteps(Card c, int knob);

// Sets the engine field a knob drives. Continuous: value is 0..1. Stepped: value is the position
// index, clamped to the knob's positions. An out-of-range card or knob does nothing.
void applyKnob(Card c, int knob, float value, ChainParams& out);

// Sets the on flag of the stage a card owns.
void applyToggle(Card c, bool on, ChainParams& out);

// One-time engine selection when a card lands in a slot (reverb engine). Safe to call every apply.
void applyPlacement(Card c, ChainParams& out);

// The descriptor of knob `knob` on card `c`, or nullptr when the card has no such knob.
const KnobDesc* knobDesc(Card c, int knob);

// The number the OLED shows for a reading `value` (0..1, or the step index for a stepped knob), in
// whole steps of 10^-decimals of the unit. A stepped value is clamped to the knob's positions. A
// knob with labels shows a name, so this is the index of that name.
int knobShown(const KnobDesc& k, float value);

// The most character cells a knob's number takes.
constexpr int kMaxKnobCells = 12;

// The character cells the number of knob `k` takes on the OLED, in the widest case. A knob whose
// range reaches below zero has a sign cell first. The digit cells follow, enough for the widest
// number of the range, and a knob with decimals adds a cell for the point and one for each decimal.
// 0 for a knob with labels. Never more than kMaxKnobCells.
int knobCells(const KnobDesc& k);

// Prints `shown` into exactly knobCells(k) characters, so every value of a knob fills the same
// cells. The sign goes in the sign cell, which stays blank for zero. A knob without a sign cell
// prints a negative number without its minus. The digits are right-aligned and the cells left of
// them stay blank. A number wider than its cells keeps its rightmost characters. Writes an empty
// string for a knob with labels or when `size` is too small.
void formatCells(const KnobDesc& k, int shown, char* buf, size_t size);

// The name at index `shown` of a labelled knob, clamped to its positions. "" for any other knob.
const char* knobLabel(const KnobDesc& k, int shown);
