#include "core/layout_fade.h"

namespace {

// Exact 1.0 at full level so an idle fade changes nothing.
float gainAt(int level) { return level >= kFadeBlocks ? 1.0f : static_cast<float>(level) / kFadeBlocks; }

// Bit of the engine behind a card, 0 for Empty.
uint32_t engineBit(Card c) {
  const Engine e = engineOf(c);
  return e == kNoEngine ? 0u : 1u << static_cast<int>(e);
}

// True when the card in slot `s` reads Autotune's tracker: the Formant engine with only Empty slots
// between it and Autotune, Mob or Deepthroat with Autotune anywhere ahead. The chain walk (runFormant,
// runCrowd, the Growl stage) reads it on the same terms, whether the Autotune slot is on or off.
bool readsAutotune(const Layout& layout, int s) {
  const Engine e = engineOf(layout.card[static_cast<size_t>(s)]);
  if (e != Engine::Formant && e != Engine::Crowd && e != Engine::Growl) return false;
  bool onlyEmpty = true;
  for (int a = s - 1; a >= 0; --a) {
    const Card c = layout.card[static_cast<size_t>(a)];
    if (c == Card::Autotune) return onlyEmpty || e != Engine::Formant;
    if (c != Card::Empty) onlyEmpty = false;
  }
  return false;
}

bool placesCard(const Layout& layout, Card c) {
  for (const Card placed : layout.card) {
    if (placed == c) return true;
  }
  return false;
}

}  // namespace

LayoutFade::Block LayoutFade::ramp(bool swap) {
  Block b{gainAt(level_), 0.0f, swap, false};
  level_ += phase_ == Phase::Out ? -1 : 1;
  b.to = gainAt(level_);
  if (phase_ == Phase::In && level_ == kFadeBlocks) phase_ = Phase::Idle;
  return b;
}

LayoutFade::Block LayoutFade::next(bool differs) {
  if (held_) return nextHeld(differs);
  Block b = nextRamp(differs);
  b.silent = b.swap || (b.from == 0.0f && b.to == 0.0f);
  return b;
}

LayoutFade::Block LayoutFade::nextRamp(bool differs) {
  if (phase_ == Phase::Idle) {
    if (!differs) return {1.0f, 1.0f, false, false};
    phase_ = Phase::Out;
  } else if (phase_ == Phase::Out) {
    if (!differs) phase_ = Phase::In;
  } else if (differs) {
    phase_ = Phase::Out;  // from In or Hold
  }

  if (phase_ == Phase::Out && level_ == 0) {
    holdLeft_ = hold_;
    phase_ = hold_ > 0 ? Phase::Hold : Phase::In;
    return hold_ > 0 ? Block{0.0f, 0.0f, true, false} : ramp(true);
  }
  if (phase_ == Phase::Hold) {
    if (--holdLeft_ == 0) phase_ = Phase::In;
    return {0.0f, 0.0f, false, false};
  }
  return ramp(false);
}

// At zero a layout swaps at once, and the silent hold after the swap counts down here too, so a
// release soon after finds the engine clears done. Otherwise the phase waits at the start of the
// fade-in for the release.
LayoutFade::Block LayoutFade::nextHeld(bool differs) {
  level_ = 0;
  if (differs) {
    holdLeft_ = hold_;
    phase_ = hold_ > 0 ? Phase::Hold : Phase::In;
    return {0.0f, 0.0f, true, true};
  }
  const bool clearing = phase_ == Phase::Hold;
  if (clearing) {
    if (--holdLeft_ == 0) phase_ = Phase::In;
  } else {
    phase_ = Phase::In;
  }
  return {0.0f, 0.0f, false, clearing};
}

void LayoutFade::cut() {
  level_ = 0;
  phase_ = Phase::Hold;
  holdLeft_ = hold_ + 1;
}

uint32_t enginesToReset(const Layout& from, const Layout& to) {
  uint32_t mask = 0;
  for (int s = 0; s < kSlotCount; ++s) {
    if (from.card[s] != to.card[s]) mask |= engineBit(from.card[s]) | engineBit(to.card[s]);
  }
  return mask;
}

uint32_t enginesReadBy(const Layout& layout, int s) {
  const Card c = layout.card[static_cast<size_t>(s)];
  uint32_t mask = engineBit(c);
  if (readsAutotune(layout, s)) mask |= engineBit(Card::Autotune);
  if (c == Card::Bloom && placesCard(layout, Card::InputGate)) mask |= engineBit(Card::InputGate);
  return mask;
}

bool changeHeard(const Layout& from, const Layout& to, uint8_t liveMask, uint32_t clearing) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (!((liveMask >> s) & 1u)) continue;
    if (from.card[static_cast<size_t>(s)] != to.card[static_cast<size_t>(s)]) return true;
    const uint32_t reads = enginesReadBy(from, s);
    if (reads != enginesReadBy(to, s) || (reads & clearing) != 0) return true;
  }
  return false;
}
