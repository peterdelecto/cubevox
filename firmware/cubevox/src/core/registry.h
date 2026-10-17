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

// The value a continuous knob holds at position 0..1, in the descriptor's unit. formatKnob uses it for
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

// Readout text for the OLED, for example "-24 dB" or "F / Dm". A stepped value is clamped to the
// knob's positions; an out-of-range card or knob writes an empty string.
void formatKnob(Card c, int knob, float value, char* buf, size_t size);
