// Effect registry: the cards Adam can place in a slot, their knobs, groups and costs.
// Firmware is the only source of truth; the Mac app learns this table over `hello`.
// Spec: docs/specs/2026-10-08-effect-registry-design.md.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "slots.h"

// Table index. Ids are wire-stable once shipped; append only.
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
};

constexpr int kCardCount = 10;
constexpr int kKnobsPerCard = 2;

enum class KnobKind : uint8_t { Continuous, Stepped };
enum class Group : uint8_t { None, Pitch, Reverb };

struct KnobDesc {
  const char* name;
  KnobKind kind;
  uint8_t positions;  // stepped only; 0 for continuous
  float min;          // in `unit`; bipolar knobs have min < 0 < max
  float max;
  const char* unit;   // "", "dB", "ms", "st", "%"
};

struct CardDesc {
  const char* id;     // wire id, snake_case
  const char* name;   // OLED and app display name
  uint8_t knobCount;  // 0 for Empty
  KnobDesc knob[kKnobsPerCard];
  Group group;
  uint8_t costPercent;  // of one audio frame at peak; costs.h
  bool measured;        // false until a board ran the bench
};

const CardDesc& cardDesc(Card c);

// 0 for a continuous knob, otherwise the number of positions it snaps to.
int knobSteps(Card c, int knob);

// Sets the engine field a knob drives. Continuous: value is 0..1. Stepped: value is the position index.
void applyKnob(Card c, int knob, float value, ChainParams& out);

// Sets the on flag of the stage a card owns.
void applyToggle(Card c, bool on, ChainParams& out);

// One-time engine selection when a card lands in a slot (reverb engine). Safe to call every apply.
void applyPlacement(Card c, ChainParams& out);

// Readout text for the OLED, for example "-24 dB" or "F / Dm".
void formatKnob(Card c, int knob, float value, char* buf, size_t size);

// Card in a slot. Defaults from the slots spec; runtime layout and flash come in step 2.
Card slotCard(int slot);
