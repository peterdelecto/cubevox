#include "core/layout_fade.h"

namespace {

// Exact 1.0 at full level so an idle fade changes nothing.
float gainAt(int level) { return level >= kFadeBlocks ? 1.0f : static_cast<float>(level) / kFadeBlocks; }

// Bit of the engine behind a card, 0 for Empty.
uint32_t engineBit(Card c) {
  switch (c) {
    case Card::Empty: return 0;
    case Card::InputGate: return 1u << static_cast<int>(Engine::InputGate);
    case Card::Autotune:
    case Card::Octave: return 1u << static_cast<int>(Engine::Pitch);
    case Card::Unison: return 1u << static_cast<int>(Engine::Unison);
    case Card::Slapback: return 1u << static_cast<int>(Engine::Slapback);
    case Card::Distortion: return 1u << static_cast<int>(Engine::Distortion);
    case Card::Gate: return 1u << static_cast<int>(Engine::Gate);
    case Card::ReverbSpring:
    case Card::ReverbChasm: return 1u << static_cast<int>(Engine::Reverb);
  }
  return 0;
}

}  // namespace

LayoutFade::Block LayoutFade::ramp(bool swap) {
  Block b{gainAt(level_), 0.0f, swap};
  level_ += phase_ == Phase::Out ? -1 : 1;
  b.to = gainAt(level_);
  if (phase_ == Phase::In && level_ == kFadeBlocks) phase_ = Phase::Idle;
  return b;
}

LayoutFade::Block LayoutFade::next(bool differs) {
  if (phase_ == Phase::Idle) {
    if (!differs) return {1.0f, 1.0f, false};
    phase_ = Phase::Out;
  } else if (phase_ == Phase::Out) {
    if (!differs) phase_ = Phase::In;
  } else if (differs) {
    phase_ = Phase::Out;  // from In or Hold
  }

  if (phase_ == Phase::Out && level_ == 0) {
    holdLeft_ = hold_;
    phase_ = hold_ > 0 ? Phase::Hold : Phase::In;
    return hold_ > 0 ? Block{0.0f, 0.0f, true} : ramp(true);
  }
  if (phase_ == Phase::Hold) {
    if (--holdLeft_ == 0) phase_ = Phase::In;
    return {0.0f, 0.0f, false};
  }
  return ramp(false);
}

uint32_t enginesToReset(const Layout& from, const Layout& to) {
  uint32_t mask = 0;
  for (int s = 0; s < kSlotCount; ++s) {
    if (from.card[s] != to.card[s]) mask |= engineBit(from.card[s]) | engineBit(to.card[s]);
  }
  return mask;
}
