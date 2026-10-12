#include "core/layout.h"

Layout defaultLayout() {
  return {{Card::InputGate, Card::Autotune, Card::Octave, Card::Unison,
           Card::Slapback, Card::Distortion, Card::Gate, Card::ReverbSpring}};
}

bool layoutValid(const Layout& layout) {
  int avg = costs::kFixed.avg;
  int worst = costs::kFixed.worst;
  int reverbs = 0;
  uint32_t placed = 0;  // bit per non-Empty card already seen
  for (const Card c : layout.card) {
    if (static_cast<int>(c) >= kCardCount) return false;
    if (c != Card::Empty) {
      const uint32_t bit = 1u << static_cast<int>(c);
      if (placed & bit) return false;
      placed |= bit;
    }
    const CardDesc& d = cardDesc(c);
    avg += d.cost.avg;
    worst += d.cost.worst;
    if (d.group == Group::Reverb) ++reverbs;
  }
  return reverbs <= 1 && avg <= costs::kBudgetAvgPercent && worst <= costs::kBudgetWorstPercent;
}
